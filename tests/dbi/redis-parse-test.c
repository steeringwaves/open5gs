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

#define OGS_DBI_COMPILATION
#include "ogs-dbi.h"
#include "ogs-dbi-backend.h"
#include "redis/redis-internal.h"
#include "core/abts.h"

/* redis_parse_uri is exposed (non-static) for testing; see redis-backend.c */
int redis_parse_uri(const char *uri, char **host, int *port, char **prefix);

/*
 * Canonical subscriber fixture, embedded as a C string constant to keep the
 * parse tests hermetic (no test-cwd dependence). This mirrors the contents of
 * tests/dbi/fixtures/subscriber.json (which the Task 8 equivalence test reads).
 * Keep the two in sync.
 */
static const char SUBSCRIBER_JSON[] =
    "{"
    "\"imsi\":\"001010000000001\","
    "\"security\":{"
        "\"k\":\"465B5CE8B199B49FAA5F0A2EE238A6BC\","
        "\"opc\":\"E8ED289DEBA952E4283B54E88E6183CA\","
        "\"amf\":\"8000\","
        "\"rand\":\"562F9BDDD952644279AE7B3957F6C3FE\","
        "\"sqn\":96"
    "},"
    "\"msisdn\":[\"491725670000\"],"
    "\"ambr\":{"
        "\"downlink\":{\"value\":1,\"unit\":3},"
        "\"uplink\":{\"value\":1,\"unit\":3}"
    "},"
    "\"slice\":[{"
        "\"sst\":1,"
        "\"default_indicator\":true,"
        "\"session\":[{"
            "\"name\":\"internet\","
            "\"type\":3,"
            "\"qos\":{"
                "\"index\":9,"
                "\"arp\":{"
                    "\"priority_level\":8,"
                    "\"pre_emption_capability\":1,"
                    "\"pre_emption_vulnerability\":1"
                "}"
            "},"
            "\"ambr\":{"
                "\"downlink\":{\"value\":1,\"unit\":3},"
                "\"uplink\":{\"value\":1,\"unit\":3}"
            "}"
        "}]"
    "}],"
    "\"access_restriction_data\":32,"
    "\"subscriber_status\":0"
    "}";

/*
 * A multi-slice / multi-session subscriber fixture. Two slices; the first slice
 * carries two sessions. Exercises the slice[] and session[] array loops and the
 * SD parse path (slice[1] has a non-default sd).
 */
static const char SUBSCRIBER_JSON_MULTI[] =
    "{"
    "\"imsi\":\"001010000000002\","
    "\"security\":{"
        "\"k\":\"465B5CE8B199B49FAA5F0A2EE238A6BC\","
        "\"opc\":\"E8ED289DEBA952E4283B54E88E6183CA\","
        "\"amf\":\"8000\","
        "\"sqn\":128"
    "},"
    "\"msisdn\":[\"491725670001\",\"491725670002\"],"
    "\"ambr\":{"
        "\"downlink\":{\"value\":1,\"unit\":3},"
        "\"uplink\":{\"value\":1,\"unit\":3}"
    "},"
    "\"slice\":[{"
        "\"sst\":1,"
        "\"default_indicator\":true,"
        "\"session\":[{"
            "\"name\":\"internet\","
            "\"type\":3,"
            "\"qos\":{\"index\":9,\"arp\":{\"priority_level\":8,"
                "\"pre_emption_capability\":1,\"pre_emption_vulnerability\":1}},"
            "\"ambr\":{\"downlink\":{\"value\":1,\"unit\":3},"
                "\"uplink\":{\"value\":1,\"unit\":3}}"
        "},{"
            "\"name\":\"ims\","
            "\"type\":3,"
            "\"qos\":{\"index\":5,\"arp\":{\"priority_level\":1,"
                "\"pre_emption_capability\":0,\"pre_emption_vulnerability\":0}},"
            "\"ambr\":{\"downlink\":{\"value\":1,\"unit\":2},"
                "\"uplink\":{\"value\":1,\"unit\":2}}"
        "}]"
    "},{"
        "\"sst\":2,"
        "\"sd\":\"000080\","
        "\"default_indicator\":false,"
        "\"session\":[{"
            "\"name\":\"data\","
            "\"type\":3,"
            "\"qos\":{\"index\":9,\"arp\":{\"priority_level\":8,"
                "\"pre_emption_capability\":1,\"pre_emption_vulnerability\":1}},"
            "\"ambr\":{\"downlink\":{\"value\":1,\"unit\":3},"
                "\"uplink\":{\"value\":1,\"unit\":3}}"
        "}]"
    "}],"
    "\"access_restriction_data\":32,"
    "\"subscriber_status\":0"
    "}";

static void uri_basic(abts_case *tc, void *data)
{
    char *host = NULL, *prefix = NULL; int port = 0;
    int rv = redis_parse_uri("redis://127.0.0.1:6380/?prefix=net:",
                             &host, &port, &prefix);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);
    ABTS_STR_EQUAL(tc, "127.0.0.1", host);
    ABTS_INT_EQUAL(tc, 6380, port);
    ABTS_STR_EQUAL(tc, "net:", prefix);
    ogs_free(host); ogs_free(prefix);
}

static void uri_defaults(abts_case *tc, void *data)
{
    char *host = NULL, *prefix = NULL; int port = 0;
    int rv = redis_parse_uri("redis://localhost", &host, &port, &prefix);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);
    ABTS_STR_EQUAL(tc, "localhost", host);
    ABTS_INT_EQUAL(tc, OGS_REDIS_DEFAULT_PORT, port);
    ABTS_STR_EQUAL(tc, OGS_REDIS_DEFAULT_PREFIX, prefix);
    ogs_free(host); ogs_free(prefix);
}

static void uri_rejects_non_redis(abts_case *tc, void *data)
{
    char *host = NULL, *prefix = NULL; int port = 0;
    int rv = redis_parse_uri("mongodb://localhost/open5gs", &host, &port, &prefix);
    ABTS_INT_EQUAL(tc, OGS_ERROR, rv);
}

static void init_selects_redis_backend(abts_case *tc, void *data)
{
    /* No server needed: redis_init returns OGS_RETRY when it can't connect,
     * but ogs_dbi_init only sets `active` on OGS_OK. So assert the backend is
     * FOUND in the registry instead. */
    ABTS_PTR_NOTNULL(tc, (void *)ogs_dbi_backend_find("redis"));
}

static void parse_auth_info_ok(abts_case *tc, void *data)
{
    cJSON *doc = cJSON_Parse(SUBSCRIBER_JSON);
    ogs_dbi_auth_info_t auth;
    int rv;
    ABTS_PTR_NOTNULL(tc, doc);
    rv = redis_parse_auth_info(doc, &auth);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);
    ABTS_INT_EQUAL(tc, 1, auth.use_opc);            /* fixture sets opc */
    /* K first byte 0x46 from "465B..." */
    ABTS_INT_EQUAL(tc, 0x46, auth.k[0]);
    ABTS_TRUE(tc, auth.sqn > 0);
    cJSON_Delete(doc);
}

static void parse_subscription_data_ok(abts_case *tc, void *data)
{
    cJSON *doc = cJSON_Parse(SUBSCRIBER_JSON);
    ogs_subscription_data_t subscription_data;
    int rv;

    ABTS_PTR_NOTNULL(tc, doc);
    rv = redis_parse_subscription_data(doc, &subscription_data);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);

    /* msisdn[] */
    ABTS_INT_EQUAL(tc, 1, subscription_data.num_of_msisdn);

    /* UE-AMBR after unit-scaling: value=1, unit=3 -> 1*1000*1000*1000 */
    ABTS_TRUE(tc, subscription_data.ambr.downlink == 1000000000ULL);
    ABTS_TRUE(tc, subscription_data.ambr.uplink == 1000000000ULL);

    /* slice[] */
    ABTS_INT_EQUAL(tc, 1, subscription_data.num_of_slice);
    ABTS_INT_EQUAL(tc, 1, subscription_data.slice[0].s_nssai.sst);
    /* No 'sd' in the single fixture -> default no-SD value */
    ABTS_INT_EQUAL(tc, OGS_S_NSSAI_NO_SD_VALUE,
            subscription_data.slice[0].s_nssai.sd.v);
    ABTS_INT_EQUAL(tc, 1, subscription_data.slice[0].default_indicator);

    /* session[] */
    ABTS_INT_EQUAL(tc, 1, subscription_data.slice[0].num_of_session);
    ABTS_PTR_NOTNULL(tc, subscription_data.slice[0].session[0].name);
    ABTS_STR_EQUAL(tc, "internet",
            subscription_data.slice[0].session[0].name);
    ABTS_INT_EQUAL(tc, OGS_PDU_SESSION_TYPE_IPV4V6,
            subscription_data.slice[0].session[0].session_type);
    ABTS_INT_EQUAL(tc, 9, subscription_data.slice[0].session[0].qos.index);
    ABTS_INT_EQUAL(tc, 8,
            subscription_data.slice[0].session[0].qos.arp.priority_level);
    ABTS_INT_EQUAL(tc, 1,
            subscription_data.slice[0].session[0].qos.arp.
                pre_emption_capability);
    ABTS_INT_EQUAL(tc, 1,
            subscription_data.slice[0].session[0].qos.arp.
                pre_emption_vulnerability);

    /* session-AMBR after unit-scaling */
    ABTS_TRUE(tc,
            subscription_data.slice[0].session[0].ambr.downlink ==
                1000000000ULL);

    ABTS_INT_EQUAL(tc, 32, (int)subscription_data.access_restriction_data);
    ABTS_INT_EQUAL(tc, 0, (int)subscription_data.subscriber_status);

    ogs_subscription_data_free(&subscription_data);
    cJSON_Delete(doc);
}

static void parse_subscription_data_multi(abts_case *tc, void *data)
{
    cJSON *doc = cJSON_Parse(SUBSCRIBER_JSON_MULTI);
    ogs_subscription_data_t subscription_data;
    int rv;

    ABTS_PTR_NOTNULL(tc, doc);
    rv = redis_parse_subscription_data(doc, &subscription_data);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);

    ABTS_INT_EQUAL(tc, 2, subscription_data.num_of_msisdn);

    /* Two slices; first carries two sessions, second carries one */
    ABTS_INT_EQUAL(tc, 2, subscription_data.num_of_slice);
    ABTS_INT_EQUAL(tc, 2, subscription_data.slice[0].num_of_session);
    ABTS_INT_EQUAL(tc, 1, subscription_data.slice[1].num_of_session);

    ABTS_INT_EQUAL(tc, 1, subscription_data.slice[0].s_nssai.sst);
    ABTS_STR_EQUAL(tc, "internet",
            subscription_data.slice[0].session[0].name);
    ABTS_STR_EQUAL(tc, "ims",
            subscription_data.slice[0].session[1].name);

    /* Second slice has an explicit sd ("000080" -> 0x80 == 128) */
    ABTS_INT_EQUAL(tc, 2, subscription_data.slice[1].s_nssai.sst);
    ABTS_INT_EQUAL(tc, 0x000080, subscription_data.slice[1].s_nssai.sd.v);
    ABTS_INT_EQUAL(tc, 0, subscription_data.slice[1].default_indicator);
    ABTS_STR_EQUAL(tc, "data",
            subscription_data.slice[1].session[0].name);

    ogs_subscription_data_free(&subscription_data);
    cJSON_Delete(doc);
}

static void parse_session_data_ok(abts_case *tc, void *data)
{
    cJSON *doc = cJSON_Parse(SUBSCRIBER_JSON);
    ogs_session_data_t session_data;
    ogs_s_nssai_t s_nssai;
    int rv;

    ABTS_PTR_NOTNULL(tc, doc);

    /* Build the s_nssai matching the fixture's single slice (sst=1, no sd). */
    memset(&s_nssai, 0, sizeof(s_nssai));
    s_nssai.sst = 1;
    s_nssai.sd.v = OGS_S_NSSAI_NO_SD_VALUE;

    memset(&session_data, 0, sizeof(session_data));
    rv = redis_parse_session_data(doc, &s_nssai, "internet", &session_data);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);

    ABTS_PTR_NOTNULL(tc, session_data.session.name);
    ABTS_STR_EQUAL(tc, "internet", session_data.session.name);
    ABTS_INT_EQUAL(tc, 9, session_data.session.qos.index);
    ABTS_INT_EQUAL(tc, OGS_PDU_SESSION_TYPE_IPV4V6,
            session_data.session.session_type);
    ABTS_TRUE(tc, session_data.session.ambr.downlink == 1000000000ULL);

    OGS_SESSION_DATA_FREE(&session_data);
    cJSON_Delete(doc);
}

static void parse_session_data_no_match(abts_case *tc, void *data)
{
    cJSON *doc = cJSON_Parse(SUBSCRIBER_JSON);
    ogs_session_data_t session_data;
    ogs_s_nssai_t s_nssai;
    int rv;

    ABTS_PTR_NOTNULL(tc, doc);

    memset(&s_nssai, 0, sizeof(s_nssai));
    s_nssai.sst = 1;
    s_nssai.sd.v = OGS_S_NSSAI_NO_SD_VALUE;

    memset(&session_data, 0, sizeof(session_data));
    rv = redis_parse_session_data(doc, &s_nssai, "nonexistent", &session_data);
    ABTS_INT_EQUAL(tc, OGS_ERROR, rv);

    OGS_SESSION_DATA_FREE(&session_data);
    cJSON_Delete(doc);
}

abts_suite *test_redis_parse(abts_suite *suite);
abts_suite *test_redis_parse(abts_suite *suite)
{
    suite = ADD_SUITE(suite);
    abts_run_test(suite, uri_basic, NULL);
    abts_run_test(suite, uri_defaults, NULL);
    abts_run_test(suite, uri_rejects_non_redis, NULL);
    abts_run_test(suite, init_selects_redis_backend, NULL);
    abts_run_test(suite, parse_auth_info_ok, NULL);
    abts_run_test(suite, parse_subscription_data_ok, NULL);
    abts_run_test(suite, parse_subscription_data_multi, NULL);
    abts_run_test(suite, parse_session_data_ok, NULL);
    abts_run_test(suite, parse_session_data_no_match, NULL);
    return suite;
}
