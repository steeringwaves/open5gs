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
 * The Docker-gated integration case below drives the Redis backend through the
 * public ogs_dbi_* API to cross-check what the open5gs-dbctl-redis CLI wrote.
 * OGS_DBI_COMPILATION unlocks the backend-internal declarations (mirrors
 * tests/dbi/redis-equivalence-test.c); it must be defined before ogs-dbi.h.
 */
#define OGS_DBI_COMPILATION
#include "ogs-core.h"
#include "ogs-dbi.h"
#include "core/abts.h"

#include "cJSON.h"
#include "dbctl-subscriber.h"

/*
 * Unit test for the pure subscriber JSON builder. No Redis is involved: we
 * build a doc and assert its shape via cJSON. The field names/nesting must
 * match what lib/dbi/redis/redis-subscription.c reads.
 */
static void test_build_subscriber(abts_case *tc, void *data)
{
    dbctl_add_args_t args;
    cJSON *doc, *security, *item, *slice, *slice0, *session, *session0;
    cJSON *qos, *arp, *ambr, *dl, *ul;

    memset(&args, 0, sizeof(args));
    args.imsi = "001010000000001";
    args.k = "465B5CE8B199B49FAA5F0A2EE238A6BC";
    args.opc = "E8ED289DEBA952E4283B54E88E6183CA";
    args.use_opc = 1;
    args.apn = "internet";
    args.sst = 1;

    doc = dbctl_build_subscriber(&args);
    ABTS_PTR_NOTNULL(tc, doc);

    /* top-level imsi */
    item = cJSON_GetObjectItemCaseSensitive(doc, "imsi");
    ABTS_PTR_NOTNULL(tc, item);
    ABTS_TRUE(tc, cJSON_IsString(item));
    ABTS_STR_EQUAL(tc, "001010000000001", item->valuestring);

    /* security.k present and security.opc present (use_opc=1) */
    security = cJSON_GetObjectItemCaseSensitive(doc, "security");
    ABTS_PTR_NOTNULL(tc, security);
    ABTS_TRUE(tc, cJSON_IsObject(security));

    item = cJSON_GetObjectItemCaseSensitive(security, "k");
    ABTS_PTR_NOTNULL(tc, item);
    ABTS_TRUE(tc, cJSON_IsString(item));
    ABTS_STR_EQUAL(tc, "465B5CE8B199B49FAA5F0A2EE238A6BC", item->valuestring);

    item = cJSON_GetObjectItemCaseSensitive(security, "opc");
    ABTS_PTR_NOTNULL(tc, item);
    ABTS_TRUE(tc, cJSON_IsString(item));
    ABTS_STR_EQUAL(tc, "E8ED289DEBA952E4283B54E88E6183CA", item->valuestring);

    /* use_opc => no security.op */
    item = cJSON_GetObjectItemCaseSensitive(security, "op");
    ABTS_PTR_EQUAL(tc, NULL, item);

    /* amf default and sqn 0 */
    item = cJSON_GetObjectItemCaseSensitive(security, "amf");
    ABTS_PTR_NOTNULL(tc, item);
    ABTS_STR_EQUAL(tc, "8000", item->valuestring);

    item = cJSON_GetObjectItemCaseSensitive(security, "sqn");
    ABTS_PTR_NOTNULL(tc, item);
    ABTS_TRUE(tc, cJSON_IsNumber(item));
    ABTS_INT_EQUAL(tc, 0, (int)cJSON_GetNumberValue(item));

    /* access_restriction_data == 32, subscriber_status == 0 */
    item = cJSON_GetObjectItemCaseSensitive(doc, "access_restriction_data");
    ABTS_PTR_NOTNULL(tc, item);
    ABTS_TRUE(tc, cJSON_IsNumber(item));
    ABTS_INT_EQUAL(tc, 32, (int)cJSON_GetNumberValue(item));

    item = cJSON_GetObjectItemCaseSensitive(doc, "subscriber_status");
    ABTS_PTR_NOTNULL(tc, item);
    ABTS_TRUE(tc, cJSON_IsNumber(item));
    ABTS_INT_EQUAL(tc, 0, (int)cJSON_GetNumberValue(item));

    /* slice is an array of size 1 */
    slice = cJSON_GetObjectItemCaseSensitive(doc, "slice");
    ABTS_PTR_NOTNULL(tc, slice);
    ABTS_TRUE(tc, cJSON_IsArray(slice));
    ABTS_INT_EQUAL(tc, 1, cJSON_GetArraySize(slice));

    slice0 = cJSON_GetArrayItem(slice, 0);
    ABTS_PTR_NOTNULL(tc, slice0);

    /* slice[0].sst == 1 */
    item = cJSON_GetObjectItemCaseSensitive(slice0, "sst");
    ABTS_PTR_NOTNULL(tc, item);
    ABTS_TRUE(tc, cJSON_IsNumber(item));
    ABTS_INT_EQUAL(tc, 1, (int)cJSON_GetNumberValue(item));

    /* slice[0].default_indicator == true */
    item = cJSON_GetObjectItemCaseSensitive(slice0, "default_indicator");
    ABTS_PTR_NOTNULL(tc, item);
    ABTS_TRUE(tc, cJSON_IsTrue(item));

    /* no SD when has_sd is unset */
    item = cJSON_GetObjectItemCaseSensitive(slice0, "sd");
    ABTS_PTR_EQUAL(tc, NULL, item);

    /* slice[0].session[0].name == "internet" */
    session = cJSON_GetObjectItemCaseSensitive(slice0, "session");
    ABTS_PTR_NOTNULL(tc, session);
    ABTS_TRUE(tc, cJSON_IsArray(session));
    ABTS_INT_EQUAL(tc, 1, cJSON_GetArraySize(session));

    session0 = cJSON_GetArrayItem(session, 0);
    ABTS_PTR_NOTNULL(tc, session0);

    item = cJSON_GetObjectItemCaseSensitive(session0, "name");
    ABTS_PTR_NOTNULL(tc, item);
    ABTS_TRUE(tc, cJSON_IsString(item));
    ABTS_STR_EQUAL(tc, "internet", item->valuestring);

    /* session[0].type == 3 (IPv4v6) */
    item = cJSON_GetObjectItemCaseSensitive(session0, "type");
    ABTS_PTR_NOTNULL(tc, item);
    ABTS_INT_EQUAL(tc, 3, (int)cJSON_GetNumberValue(item));

    /* session[0].qos.index == 9 */
    qos = cJSON_GetObjectItemCaseSensitive(session0, "qos");
    ABTS_PTR_NOTNULL(tc, qos);
    ABTS_TRUE(tc, cJSON_IsObject(qos));

    item = cJSON_GetObjectItemCaseSensitive(qos, "index");
    ABTS_PTR_NOTNULL(tc, item);
    ABTS_INT_EQUAL(tc, 9, (int)cJSON_GetNumberValue(item));

    /* session[0].qos.arp.priority_level == 8 */
    arp = cJSON_GetObjectItemCaseSensitive(qos, "arp");
    ABTS_PTR_NOTNULL(tc, arp);
    ABTS_TRUE(tc, cJSON_IsObject(arp));

    item = cJSON_GetObjectItemCaseSensitive(arp, "priority_level");
    ABTS_PTR_NOTNULL(tc, item);
    ABTS_INT_EQUAL(tc, 8, (int)cJSON_GetNumberValue(item));

    item = cJSON_GetObjectItemCaseSensitive(arp, "pre_emption_capability");
    ABTS_PTR_NOTNULL(tc, item);
    ABTS_INT_EQUAL(tc, 1, (int)cJSON_GetNumberValue(item));

    item = cJSON_GetObjectItemCaseSensitive(arp, "pre_emption_vulnerability");
    ABTS_PTR_NOTNULL(tc, item);
    ABTS_INT_EQUAL(tc, 1, (int)cJSON_GetNumberValue(item));

    /* session[0].ambr (APN-AMBR) has downlink/uplink {value,unit} */
    ambr = cJSON_GetObjectItemCaseSensitive(session0, "ambr");
    ABTS_PTR_NOTNULL(tc, ambr);
    ABTS_TRUE(tc, cJSON_IsObject(ambr));
    ABTS_PTR_NOTNULL(tc, cJSON_GetObjectItemCaseSensitive(ambr, "downlink"));
    ABTS_PTR_NOTNULL(tc, cJSON_GetObjectItemCaseSensitive(ambr, "uplink"));

    /*
     * Top-level ambr (UE-AMBR) must exist with downlink/uplink {value,unit}.
     * The Redis reader maps this onto subscription_data->ambr, which the HSS
     * advertises as UE-AMBR in S6a ULA/IDR; without it the HSS sends 0/0.
     */
    ambr = cJSON_GetObjectItemCaseSensitive(doc, "ambr");
    ABTS_PTR_NOTNULL(tc, ambr);
    ABTS_TRUE(tc, cJSON_IsObject(ambr));

    dl = cJSON_GetObjectItemCaseSensitive(ambr, "downlink");
    ABTS_PTR_NOTNULL(tc, dl);
    ABTS_TRUE(tc, cJSON_IsObject(dl));
    item = cJSON_GetObjectItemCaseSensitive(dl, "value");
    ABTS_PTR_NOTNULL(tc, item);
    ABTS_INT_EQUAL(tc, 1, (int)cJSON_GetNumberValue(item));
    item = cJSON_GetObjectItemCaseSensitive(dl, "unit");
    ABTS_PTR_NOTNULL(tc, item);
    ABTS_INT_EQUAL(tc, 3, (int)cJSON_GetNumberValue(item));

    ul = cJSON_GetObjectItemCaseSensitive(ambr, "uplink");
    ABTS_PTR_NOTNULL(tc, ul);
    ABTS_TRUE(tc, cJSON_IsObject(ul));
    item = cJSON_GetObjectItemCaseSensitive(ul, "value");
    ABTS_PTR_NOTNULL(tc, item);
    ABTS_INT_EQUAL(tc, 1, (int)cJSON_GetNumberValue(item));
    item = cJSON_GetObjectItemCaseSensitive(ul, "unit");
    ABTS_PTR_NOTNULL(tc, item);
    ABTS_INT_EQUAL(tc, 3, (int)cJSON_GetNumberValue(item));

    /* dbctl_subscriber_imsi returns the imsi */
    ABTS_STR_EQUAL(tc, "001010000000001", dbctl_subscriber_imsi(doc));

    cJSON_Delete(doc);
}

/*
 * When --op is chosen (use_opc=0), the doc must carry security.op and not opc.
 * Also exercises sd/msisdn emission and the apn default.
 */
static void test_build_subscriber_op_sd_msisdn(abts_case *tc, void *data)
{
    dbctl_add_args_t args;
    cJSON *doc, *security, *item, *slice0, *msisdn;

    memset(&args, 0, sizeof(args));
    args.imsi = "001010000000002";
    args.k = "465B5CE8B199B49FAA5F0A2EE238A6BC";
    args.op = "C42449363BBAD02B66D16BC975D77CC1";
    args.use_opc = 0;
    args.sst = 1;
    args.sd = "000001";
    args.has_sd = 1;
    args.msisdn = "491725670000";
    /* apn left NULL -> defaults to "internet" */

    doc = dbctl_build_subscriber(&args);
    ABTS_PTR_NOTNULL(tc, doc);

    security = cJSON_GetObjectItemCaseSensitive(doc, "security");
    ABTS_PTR_NOTNULL(tc, security);

    item = cJSON_GetObjectItemCaseSensitive(security, "op");
    ABTS_PTR_NOTNULL(tc, item);
    ABTS_STR_EQUAL(tc, "C42449363BBAD02B66D16BC975D77CC1", item->valuestring);

    item = cJSON_GetObjectItemCaseSensitive(security, "opc");
    ABTS_PTR_EQUAL(tc, NULL, item);

    slice0 = cJSON_GetArrayItem(
            cJSON_GetObjectItemCaseSensitive(doc, "slice"), 0);
    ABTS_PTR_NOTNULL(tc, slice0);

    item = cJSON_GetObjectItemCaseSensitive(slice0, "sd");
    ABTS_PTR_NOTNULL(tc, item);
    ABTS_STR_EQUAL(tc, "000001", item->valuestring);

    /* apn defaulted */
    item = cJSON_GetObjectItemCaseSensitive(
            cJSON_GetArrayItem(
                cJSON_GetObjectItemCaseSensitive(slice0, "session"), 0),
            "name");
    ABTS_PTR_NOTNULL(tc, item);
    ABTS_STR_EQUAL(tc, "internet", item->valuestring);

    /* msisdn is a one-element array */
    msisdn = cJSON_GetObjectItemCaseSensitive(doc, "msisdn");
    ABTS_PTR_NOTNULL(tc, msisdn);
    ABTS_TRUE(tc, cJSON_IsArray(msisdn));
    ABTS_INT_EQUAL(tc, 1, cJSON_GetArraySize(msisdn));
    item = cJSON_GetArrayItem(msisdn, 0);
    ABTS_PTR_NOTNULL(tc, item);
    ABTS_STR_EQUAL(tc, "491725670000", item->valuestring);

    cJSON_Delete(doc);
}

/*
 * The rich change-event payload builder. Its shape must match what the Phase-3
 * watcher parses in lib/dbi/redis/redis-watch.c (redis_parse_rich_event):
 * {"imsi":"<imsi>","fields":["ambr","slice",...]} and, when no fields are
 * given, {"imsi":"<imsi>"} with NO "fields" key (the watcher reads that as a
 * full ALL-fields refresh).
 */
static void test_build_change_payload(abts_case *tc, void *data)
{
    char *json;
    cJSON *doc, *imsi, *fields, *f;
    const char *two[] = { "ambr", "slice" };

    /* (a) imsi + explicit fields -> {"imsi":...,"fields":["ambr","slice"]} */
    json = dbctl_build_change_payload("001010000000001", two, 2);
    ABTS_PTR_NOTNULL(tc, json);
    doc = cJSON_Parse(json);
    ABTS_PTR_NOTNULL(tc, doc);

    imsi = cJSON_GetObjectItemCaseSensitive(doc, "imsi");
    ABTS_PTR_NOTNULL(tc, imsi);
    ABTS_TRUE(tc, cJSON_IsString(imsi));
    ABTS_STR_EQUAL(tc, "001010000000001", imsi->valuestring);

    fields = cJSON_GetObjectItemCaseSensitive(doc, "fields");
    ABTS_PTR_NOTNULL(tc, fields);
    ABTS_TRUE(tc, cJSON_IsArray(fields));
    ABTS_INT_EQUAL(tc, 2, cJSON_GetArraySize(fields));
    f = cJSON_GetArrayItem(fields, 0);
    ABTS_PTR_NOTNULL(tc, f);
    ABTS_STR_EQUAL(tc, "ambr", f->valuestring);
    f = cJSON_GetArrayItem(fields, 1);
    ABTS_PTR_NOTNULL(tc, f);
    ABTS_STR_EQUAL(tc, "slice", f->valuestring);

    cJSON_Delete(doc);
    cJSON_free(json);

    /* (b) no fields (NULL,0) -> {"imsi":...} with NO "fields" key (ALL). */
    json = dbctl_build_change_payload("001010000000002", NULL, 0);
    ABTS_PTR_NOTNULL(tc, json);
    doc = cJSON_Parse(json);
    ABTS_PTR_NOTNULL(tc, doc);

    imsi = cJSON_GetObjectItemCaseSensitive(doc, "imsi");
    ABTS_PTR_NOTNULL(tc, imsi);
    ABTS_STR_EQUAL(tc, "001010000000002", imsi->valuestring);

    fields = cJSON_GetObjectItemCaseSensitive(doc, "fields");
    ABTS_PTR_EQUAL(tc, NULL, fields);

    cJSON_Delete(doc);
    cJSON_free(json);

    /* (c) a non-NULL array but nfields == 0 also omits "fields". */
    json = dbctl_build_change_payload("001010000000003", two, 0);
    ABTS_PTR_NOTNULL(tc, json);
    doc = cJSON_Parse(json);
    ABTS_PTR_NOTNULL(tc, doc);
    fields = cJSON_GetObjectItemCaseSensitive(doc, "fields");
    ABTS_PTR_EQUAL(tc, NULL, fields);
    cJSON_Delete(doc);
    cJSON_free(json);

    /* (d) NULL imsi -> NULL (no payload). */
    json = dbctl_build_change_payload(NULL, two, 2);
    ABTS_PTR_EQUAL(tc, NULL, json);
}

/*
 * The MongoDB Extended-JSON canonicalizer. mongoexport emits wrappers like
 * {"$numberLong":"96"}, {"$oid":"66b..."}, {"$numberInt":"5"} and
 * {"$date":...}; dbctl_canonicalize_extended_json() unwraps them in place so
 * the Phase-2 reader (which expects plain JSON) reads the imported docs. Here
 * we assert the unwrap works recursively (objects, nested objects, arrays).
 */
static void test_canonicalize_extended_json(abts_case *tc, void *data)
{
    cJSON *doc, *security, *sqn, *id, *n, *nested, *nested0, *v;

    doc = cJSON_Parse(
        "{"
            "\"imsi\":\"001010000000001\","
            "\"security\":{\"sqn\":{\"$numberLong\":\"96\"}},"
            "\"_id\":{\"$oid\":\"66b0000000000000000000aa\"},"
            "\"n\":{\"$numberInt\":\"5\"},"
            "\"nested\":[{\"v\":{\"$numberLong\":\"7\"}}]"
        "}");
    ABTS_PTR_NOTNULL(tc, doc);

    dbctl_canonicalize_extended_json(doc);

    /* security.sqn is now a plain number == 96 */
    security = cJSON_GetObjectItemCaseSensitive(doc, "security");
    ABTS_PTR_NOTNULL(tc, security);
    ABTS_TRUE(tc, cJSON_IsObject(security));
    sqn = cJSON_GetObjectItemCaseSensitive(security, "sqn");
    ABTS_PTR_NOTNULL(tc, sqn);
    ABTS_TRUE(tc, cJSON_IsNumber(sqn));
    ABTS_INT_EQUAL(tc, 96, (int)cJSON_GetNumberValue(sqn));

    /* _id is unwrapped from {$oid} to its plain string */
    id = cJSON_GetObjectItemCaseSensitive(doc, "_id");
    ABTS_PTR_NOTNULL(tc, id);
    ABTS_TRUE(tc, cJSON_IsString(id));
    ABTS_STR_EQUAL(tc, "66b0000000000000000000aa", id->valuestring);

    /* n is unwrapped from {$numberInt} to a plain number == 5 */
    n = cJSON_GetObjectItemCaseSensitive(doc, "n");
    ABTS_PTR_NOTNULL(tc, n);
    ABTS_TRUE(tc, cJSON_IsNumber(n));
    ABTS_INT_EQUAL(tc, 5, (int)cJSON_GetNumberValue(n));

    /* nested[0].v is unwrapped inside an array element == 7 */
    nested = cJSON_GetObjectItemCaseSensitive(doc, "nested");
    ABTS_PTR_NOTNULL(tc, nested);
    ABTS_TRUE(tc, cJSON_IsArray(nested));
    ABTS_INT_EQUAL(tc, 1, cJSON_GetArraySize(nested));
    nested0 = cJSON_GetArrayItem(nested, 0);
    ABTS_PTR_NOTNULL(tc, nested0);
    v = cJSON_GetObjectItemCaseSensitive(nested0, "v");
    ABTS_PTR_NOTNULL(tc, v);
    ABTS_TRUE(tc, cJSON_IsNumber(v));
    ABTS_INT_EQUAL(tc, 7, (int)cJSON_GetNumberValue(v));

    cJSON_Delete(doc);
}

/*
 * ---------------------------------------------------------------------------
 * Docker-gated CLI<->backend cross-check (the key integration test).
 *
 * The companion runner tests/dbctl/run.sh spins up redis:7-alpine, runs
 *   open5gs-dbctl-redis ... add --imsi 001010000000001 --key 465B... --opc ...
 *       --apn internet --sst 1 --msisdn 491725670000
 * then exports OGS_TEST_REDIS_URI=redis://127.0.0.1:<port>/?prefix=test: and
 * runs this suite. This case `ogs_dbi_init`s the SAME Redis/prefix and reads the
 * subscriber back through the public ogs_dbi_* API, asserting the data matches
 * exactly what the CLI's `add` wrote. That proves CLI-writes == NF-reads, which
 * is the highest-value guard against key/shape drift between the tool and the
 * backend reader.
 *
 * Gated on OGS_TEST_REDIS_URI: when unset/empty the case skips cleanly (ABTS
 * pass), so `meson test --suite dbctl` stays green without Docker/Redis.
 * ---------------------------------------------------------------------------
 */
#define IT_SUPI   "imsi-001010000000001"
#define IT_IMSI   "001010000000001"

static void test_cli_backend_readback(abts_case *tc, void *data)
{
    const char *uri = getenv("OGS_TEST_REDIS_URI");
    ogs_dbi_auth_info_t auth;
    ogs_subscription_data_t subscription_data;
    int rv;

    if (!uri || uri[0] == '\0') {
        ogs_info("dbctl CLI<->backend read-back test skipped: "
                "set OGS_TEST_REDIS_URI to run (tests/dbctl/run.sh does this)");
        ABTS_TRUE(tc, 1);
        return;
    }

    ogs_info("dbctl CLI<->backend read-back test running against %s", uri);

    rv = ogs_dbi_init(uri);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);
    if (rv != OGS_OK) {
        ogs_error("ogs_dbi_init(%s) failed; is Redis reachable?", uri);
        return;
    }

    /*
     * Read 1 (auth_info): the CLI `add --key 465B... --opc ...` must surface as
     * k[0]==0x46 and use_opc==1. A freshly-added subscriber starts at sqn 0.
     */
    memset(&auth, 0, sizeof(auth));
    rv = ogs_dbi_auth_info(IT_SUPI, &auth);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);
    ABTS_INT_EQUAL(tc, 0x46, auth.k[0]);
    ABTS_INT_EQUAL(tc, 1, auth.use_opc);
    ABTS_TRUE(tc, auth.sqn == 0);

    /*
     * Read 2 (subscription_data): the CLI `add --apn internet --sst 1` must
     * surface as one slice / one session "internet" with the webui-default
     * qos.index 9 — exactly the shape dbctl_build_subscriber emits.
     */
    memset(&subscription_data, 0, sizeof(subscription_data));
    rv = ogs_dbi_subscription_data(IT_SUPI, &subscription_data);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);
    ABTS_TRUE(tc, subscription_data.num_of_slice >= 1);
    if (subscription_data.num_of_slice >= 1) {
        ABTS_INT_EQUAL(tc, 1, subscription_data.slice[0].s_nssai.sst);
        ABTS_TRUE(tc, subscription_data.slice[0].num_of_session >= 1);
        ABTS_PTR_NOTNULL(tc, subscription_data.slice[0].session[0].name);
        ABTS_STR_EQUAL(tc, "internet",
                subscription_data.slice[0].session[0].name);
        ABTS_INT_EQUAL(tc, 9,
                subscription_data.slice[0].session[0].qos.index);
    }
    /*
     * The top-level UE-AMBR must read back as the 1 Gbps default (value 1,
     * unit 3 -> 1*1000^3 bps), not 0/0. This is the field the HSS advertises
     * as UE-AMBR in S6a ULA/IDR.
     */
    ABTS_TRUE(tc, subscription_data.ambr.downlink == 1000000000ULL);
    ABTS_TRUE(tc, subscription_data.ambr.uplink == 1000000000ULL);
    ogs_subscription_data_free(&subscription_data);

    ogs_dbi_final();
}

abts_suite *test_dbctl_subscriber(abts_suite *suite)
{
    suite = ADD_SUITE(suite)

    abts_run_test(suite, test_build_subscriber, NULL);
    abts_run_test(suite, test_build_subscriber_op_sd_msisdn, NULL);
    abts_run_test(suite, test_build_change_payload, NULL);
    abts_run_test(suite, test_canonicalize_extended_json, NULL);
    abts_run_test(suite, test_cli_backend_readback, NULL);

    return suite;
}
