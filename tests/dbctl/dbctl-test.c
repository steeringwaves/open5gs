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

#include "ogs-core.h"
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
    cJSON *qos, *arp;

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

abts_suite *test_dbctl_subscriber(abts_suite *suite)
{
    suite = ADD_SUITE(suite)

    abts_run_test(suite, test_build_subscriber, NULL);
    abts_run_test(suite, test_build_subscriber_op_sd_msisdn, NULL);

    return suite;
}
