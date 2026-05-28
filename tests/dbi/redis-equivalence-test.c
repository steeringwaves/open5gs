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
#include "ogs-dbi-backend.h"
/*
 * The watcher integration test below opens a SECOND hiredis connection (the
 * subscriber connection is owned by the backend) to drive SET/PUBLISH from the
 * "client" side, and reuses the non-static redis_parse_uri to recover the
 * host/port/prefix from OGS_TEST_REDIS_URI. Both come from the redis backend
 * internal header, which also pulls in <hiredis.h>.
 */
#include "redis/redis-internal.h"
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
     * Read 3b: msisdn_data resolved by the BARE IMSI value (no `imsi-` type
     * prefix and no MSISDN index entry). This mirrors hss-cx-path callers and
     * mongoc's `$or [imsi, msisdn]`. The lookup must fall back to the
     * <prefix>subscriber:<value> key directly and resolve the SAME subscriber.
     */
    memset(&msisdn_data, 0, sizeof(msisdn_data));
    rv = ogs_dbi_msisdn_data(EQ_IMSI, &msisdn_data);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);
    ABTS_STR_EQUAL(tc, EQ_IMSI, msisdn_data.imsi.bcd);
    ABTS_TRUE(tc, msisdn_data.num_of_msisdn >= 1);

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

/*
 * ---------------------------------------------------------------------------
 * Live change-notification watcher test (Docker-gated).
 *
 * Proves the Redis watcher delivers BOTH channels through the Phase-1 neutral
 * callback (the same callback HSS registers):
 *   - keyspace fallback: a third-party `SET <prefix>subscriber:<imsi>` produces
 *     an OGS_DBI_FIELD_ALL change event for that IMSI.
 *   - rich channel: a `PUBLISH <prefix>events:subscriber {"imsi":...,
 *     "fields":["ambr","slice"]}` produces a precise (AMBR|SLICE) change event.
 *
 * The SET/PUBLISH are issued from a SECOND hiredis connection (the backend's
 * sub_ctx is subscriber-only and can't issue regular commands), connecting to
 * the same host/port recovered by re-parsing OGS_TEST_REDIS_URI.
 * ---------------------------------------------------------------------------
 */

/* Minimal but valid subscriber JSON for the keyspace SET (value is not parsed
 * by the watcher; only the keyspace 'set' event matters). */
#define WATCH_SET_JSON "{\"imsi\":\"" EQ_IMSI "\"}"
/* Rich event payload: precise field list -> (AMBR|SLICE). */
#define WATCH_RICH_JSON \
    "{\"imsi\":\"" EQ_IMSI "\",\"fields\":[\"ambr\",\"slice\"]}"

static struct {
    char imsi[OGS_MAX_IMSI_BCD_LEN + 1];
    uint32_t mask;
    int count;
} g_captured;

static void capture_cb(ogs_dbi_change_event_t *event, void *data)
{
    if (event && event->imsi_bcd) {
        ogs_cpystrn(g_captured.imsi, event->imsi_bcd, sizeof(g_captured.imsi));
        g_captured.mask = event->updated_fields_mask;
        g_captured.count++;
    }
    ogs_dbi_change_event_free(event);   /* handler owns the event */
}

static void capture_reset(void)
{
    memset(&g_captured, 0, sizeof(g_captured));
}

/* Pump the watcher poll up to `max` ticks, stopping as soon as an event has
 * been captured. Always poll at least once. */
static void poll_until_captured(int max)
{
    int i;
    for (i = 0; i < max; i++) {
        ogs_dbi_poll_change_stream();
        if (g_captured.count > 0)
            break;
        ogs_msleep(50);
    }
}

static void watch_keyspace_and_rich(abts_case *tc, void *data)
{
    const char *uri = getenv("OGS_TEST_REDIS_URI");
    char *host = NULL, *prefix = NULL;
    int port = 0;
    redisContext *c2 = NULL;
    redisReply *reply;
    int rv, i;

    if (!uri || uri[0] == '\0') {
        ogs_info("redis watcher test skipped: "
                "set OGS_TEST_REDIS_URI to run");
        ABTS_TRUE(tc, 1);
        return;
    }

    ogs_info("redis watcher test running against %s", uri);

    /* Recover host/port/prefix for the second (client) connection. */
    rv = redis_parse_uri(uri, &host, &port, &prefix);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);
    if (rv != OGS_OK)
        return;
    ABTS_PTR_NOTNULL(tc, host);
    ABTS_PTR_NOTNULL(tc, prefix);
    ogs_info("redis watcher: host=%s port=%d prefix=%s", host, port, prefix);

    rv = ogs_dbi_init(uri);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);
    if (rv != OGS_OK) {
        ogs_error("ogs_dbi_init(%s) failed; is Redis reachable?", uri);
        ogs_free(host);
        ogs_free(prefix);
        return;
    }

    ogs_dbi_set_change_handler(capture_cb, NULL);

    rv = ogs_dbi_collection_watch_init();
    ABTS_INT_EQUAL(tc, OGS_OK, rv);
    if (rv != OGS_OK) {
        ogs_error("ogs_dbi_collection_watch_init() failed");
        ogs_dbi_final();
        ogs_dbi_set_change_handler(NULL, NULL);
        ogs_free(host);
        ogs_free(prefix);
        return;
    }

    /* Pump a few times to consume the SUBSCRIBE/PSUBSCRIBE confirmations and
     * let the server register the subscriptions before we publish. */
    for (i = 0; i < 5; i++) {
        ogs_dbi_poll_change_stream();
        ogs_msleep(50);
    }

    /* Second connection (client side) for SET/PUBLISH. */
    c2 = redisConnect(host, port);
    ABTS_PTR_NOTNULL(tc, c2);
    if (!c2 || c2->err) {
        ogs_error("redis watcher: second connection failed: %s",
                c2 ? c2->errstr : "alloc");
        ABTS_TRUE(tc, 0);
        if (c2) redisFree(c2);
        ogs_dbi_final();
        ogs_dbi_set_change_handler(NULL, NULL);
        ogs_free(host);
        ogs_free(prefix);
        return;
    }

    /*
     * Keyspace path: SET <prefix>subscriber:<imsi> -> keyspace 'set' event ->
     * OGS_DBI_FIELD_ALL.
     */
    capture_reset();
    reply = redisCommand(c2, "SET %ssubscriber:%s %s",
            prefix, EQ_IMSI, WATCH_SET_JSON);
    ABTS_PTR_NOTNULL(tc, reply);
    if (reply) freeReplyObject(reply);

    poll_until_captured(20);

    ABTS_TRUE(tc, g_captured.count > 0);
    ABTS_STR_EQUAL(tc, EQ_IMSI, g_captured.imsi);
    ABTS_INT_EQUAL(tc, (int)OGS_DBI_FIELD_ALL, (int)g_captured.mask);
    ogs_info("redis watcher: keyspace event captured "
            "(imsi=%s mask=0x%x count=%d)",
            g_captured.imsi, g_captured.mask, g_captured.count);

    /*
     * Rich path: PUBLISH <prefix>events:subscriber {...fields...} ->
     * precise (AMBR|SLICE) mask.
     */
    capture_reset();
    reply = redisCommand(c2, "PUBLISH %sevents:subscriber %s",
            prefix, WATCH_RICH_JSON);
    ABTS_PTR_NOTNULL(tc, reply);
    if (reply) freeReplyObject(reply);

    poll_until_captured(20);

    ABTS_TRUE(tc, g_captured.count > 0);
    ABTS_STR_EQUAL(tc, EQ_IMSI, g_captured.imsi);
    ABTS_INT_EQUAL(tc,
            (int)(OGS_DBI_FIELD_AMBR | OGS_DBI_FIELD_SLICE),
            (int)g_captured.mask);
    ogs_info("redis watcher: rich event captured "
            "(imsi=%s mask=0x%x count=%d)",
            g_captured.imsi, g_captured.mask, g_captured.count);

    redisFree(c2);
    ogs_dbi_final();
    ogs_dbi_set_change_handler(NULL, NULL);
    ogs_free(host);
    ogs_free(prefix);
}

abts_suite *test_redis_equivalence(abts_suite *suite);
abts_suite *test_redis_equivalence(abts_suite *suite)
{
    suite = ADD_SUITE(suite);
    abts_run_test(suite, equivalence_roundtrip, NULL);
    abts_run_test(suite, watch_keyspace_and_rich, NULL);
    return suite;
}
