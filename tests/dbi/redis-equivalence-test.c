/*
 * Copyright (C) 2026 by Sukchan Lee <acetcom@gmail.com>
 *
 * This file is part of Open5GS.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * Docker-gated equivalence + SQN round-trip integration test.
 *
 * This test exercises the full ogs_dbi_* dispatch path against a REAL Redis
 * server, proving the Redis backend reads the same canonical subscriber the
 * unit tests parse, and that the SQN writers persist through Redis.
 *
 * It is gated behind the OGS_TEST_REDIS_URI environment variable so the suite
 * stays green without Docker/Redis: when the variable is unset/empty the test
 * skips cleanly (ABTS pass). The companion helper tests/dbi/run-redis-tests.sh
 * spins up a redis:7-alpine container, provisions the fixture via redis-cli,
 * exports OGS_TEST_REDIS_URI and runs this suite.
 *
 * Provisioning is done by the helper script (redis-cli SET), NOT here: this
 * test only reads/writes through the public ogs_dbi_* API.
 */

#define OGS_DBI_COMPILATION
#include "ogs-dbi.h"
#include "core/abts.h"

#define EQ_SUPI "imsi-001010000000001"
#define EQ_IMSI "001010000000001"
#define EQ_MSISDN "491725670000"

static void equivalence_roundtrip(abts_case *tc, void *data)
{
    const char *uri = getenv("OGS_TEST_REDIS_URI");
    ogs_dbi_auth_info_t auth;
    ogs_subscription_data_t subscription_data;
    ogs_msisdn_data_t msisdn_data;
    uint64_t expected_increment;
    int rv;

    if (!uri || uri[0] == '\0') {
        ogs_info("redis equivalence test skipped: "
                "set OGS_TEST_REDIS_URI to run");
        ABTS_TRUE(tc, 1);
        return;
    }

    ogs_info("redis equivalence test running against %s", uri);

    rv = ogs_dbi_init(uri);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);
    if (rv != OGS_OK) {
        ogs_error("ogs_dbi_init(%s) failed; is Redis reachable?", uri);
        return;
    }

    /*
     * Read 1: auth_info. Provisioned security{k:"465B...", opc, sqn:96}.
     */
    memset(&auth, 0, sizeof(auth));
    rv = ogs_dbi_auth_info(EQ_SUPI, &auth);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);
    ABTS_INT_EQUAL(tc, 0x46, auth.k[0]);
    ABTS_INT_EQUAL(tc, 1, auth.use_opc);
    ABTS_TRUE(tc, auth.sqn == 96);

    /*
     * Read 2: subscription_data. One slice / one session "internet" qos 9.
     */
    memset(&subscription_data, 0, sizeof(subscription_data));
    rv = ogs_dbi_subscription_data(EQ_SUPI, &subscription_data);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);
    ABTS_TRUE(tc, subscription_data.num_of_slice >= 1);
    if (subscription_data.num_of_slice >= 1) {
        ABTS_TRUE(tc, subscription_data.slice[0].num_of_session >= 1);
        ABTS_PTR_NOTNULL(tc, subscription_data.slice[0].session[0].name);
        ABTS_STR_EQUAL(tc, "internet",
                subscription_data.slice[0].session[0].name);
        ABTS_INT_EQUAL(tc, 9,
                subscription_data.slice[0].session[0].qos.index);
    }
    ogs_subscription_data_free(&subscription_data);

    /*
     * Read 3: msisdn_data resolved via the MSISDN secondary index.
     * The helper SETs <prefix>msisdn:491725670000 -> 001010000000001.
     */
    memset(&msisdn_data, 0, sizeof(msisdn_data));
    rv = ogs_dbi_msisdn_data(EQ_MSISDN, &msisdn_data);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);
    ABTS_STR_EQUAL(tc, EQ_IMSI, msisdn_data.imsi.bcd);

    /*
     * Round-trip write 1: set SQN to 200, re-read through auth_info.
     */
    rv = ogs_dbi_update_sqn(EQ_SUPI, 200);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);

    memset(&auth, 0, sizeof(auth));
    rv = ogs_dbi_auth_info(EQ_SUPI, &auth);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);
    ABTS_TRUE(tc, auth.sqn == 200);

    /*
     * Round-trip write 2: increment SQN (+32, masked with OGS_MAX_SQN).
     * 200 + 32 == 232 (well within OGS_MAX_SQN), re-read to confirm persist.
     */
    expected_increment = (200 + 32) & OGS_MAX_SQN;
    rv = ogs_dbi_increment_sqn(EQ_SUPI);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);

    memset(&auth, 0, sizeof(auth));
    rv = ogs_dbi_auth_info(EQ_SUPI, &auth);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);
    ABTS_TRUE(tc, auth.sqn == expected_increment);
    ABTS_TRUE(tc, auth.sqn == 232);

    ogs_dbi_final();
}

abts_suite *test_redis_equivalence(abts_suite *suite);
abts_suite *test_redis_equivalence(abts_suite *suite)
{
    suite = ADD_SUITE(suite);
    abts_run_test(suite, equivalence_roundtrip, NULL);
    return suite;
}
