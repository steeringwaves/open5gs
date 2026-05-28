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

#include "dbctl-subscriber.h"

#include <string.h>
#include <stdlib.h>

/*
 * The JSON keys below are the literal values of the OGS_*_STRING macros from
 * lib/proto/types.h. The Phase-2 Redis reader
 * (lib/dbi/redis/redis-subscription.c: redis_parse_auth_info /
 * redis_parse_subscription_data) keys off those same macros, so the document
 * this builder produces is read back without any field-name translation. This
 * file is kept proto-header-free (only cJSON) so the pure unit tests can link
 * against libcore alone; if a key ever changes in types.h, update it here and
 * in tests/dbctl/dbctl-test.c.
 */
#define DBCTL_IMSI_STRING                       "imsi"
#define DBCTL_MSISDN_STRING                     "msisdn"
#define DBCTL_SCHEMA_VERSION_STRING             "schema_version"
#define DBCTL_ACCESS_RESTRICTION_DATA_STRING    "access_restriction_data"
#define DBCTL_SUBSCRIBER_STATUS_STRING          "subscriber_status"
#define DBCTL_OPERATOR_DETERMINED_BARRING_STRING "operator_determined_barring"
#define DBCTL_NETWORK_ACCESS_MODE_STRING        "network_access_mode"
#define DBCTL_SUBSCRIBED_RAU_TAU_TIMER_STRING   "subscribed_rau_tau_timer"

#define DBCTL_SECURITY_STRING                   "security"
#define DBCTL_K_STRING                          "k"
#define DBCTL_OPC_STRING                        "opc"
#define DBCTL_OP_STRING                         "op"
#define DBCTL_AMF_STRING                        "amf"
#define DBCTL_RAND_STRING                       "rand"
#define DBCTL_SQN_STRING                        "sqn"

#define DBCTL_AMBR_STRING                       "ambr"
#define DBCTL_DOWNLINK_STRING                   "downlink"
#define DBCTL_UPLINK_STRING                     "uplink"
#define DBCTL_VALUE_STRING                      "value"
#define DBCTL_UNIT_STRING                       "unit"

#define DBCTL_SLICE_STRING                      "slice"
#define DBCTL_SST_STRING                        "sst"
#define DBCTL_SD_STRING                         "sd"
#define DBCTL_DEFAULT_INDICATOR_STRING          "default_indicator"
#define DBCTL_SESSION_STRING                    "session"
#define DBCTL_NAME_STRING                       "name"
#define DBCTL_TYPE_STRING                       "type"
#define DBCTL_QOS_STRING                        "qos"
#define DBCTL_INDEX_STRING                      "index"
#define DBCTL_ARP_STRING                        "arp"
#define DBCTL_PRIORITY_LEVEL_STRING             "priority_level"
#define DBCTL_PRE_EMPTION_CAPABILITY_STRING     "pre_emption_capability"
#define DBCTL_PRE_EMPTION_VULNERABILITY_STRING  "pre_emption_vulnerability"

/* webui defaults (see webui/server/models/subscriber.js) */
#define DBCTL_DEFAULT_AMF               "8000"
#define DBCTL_DEFAULT_APN               "internet"
#define DBCTL_DEFAULT_SST               1
#define DBCTL_SESSION_TYPE_IPV4V6       3
#define DBCTL_QOS_INDEX_DEFAULT         9
#define DBCTL_ARP_PRIORITY_LEVEL        8
#define DBCTL_ARP_PRE_EMPTION_CAP       1
#define DBCTL_ARP_PRE_EMPTION_VUL       1
#define DBCTL_AMBR_VALUE_DEFAULT        1
#define DBCTL_AMBR_UNIT_DEFAULT         3   /* Mbps (1000^3) */
#define DBCTL_SCHEMA_VERSION_DEFAULT    1
#define DBCTL_ACCESS_RESTRICTION_DEFAULT 32

/* Build an AMBR object {downlink:{value,unit}, uplink:{value,unit}}. */
static cJSON *build_ambr(void)
{
    cJSON *ambr, *dl, *ul;

    ambr = cJSON_CreateObject();
    if (!ambr)
        return NULL;

    dl = cJSON_AddObjectToObject(ambr, DBCTL_DOWNLINK_STRING);
    if (!dl) goto failed;
    if (!cJSON_AddNumberToObject(dl, DBCTL_VALUE_STRING,
            DBCTL_AMBR_VALUE_DEFAULT)) goto failed;
    if (!cJSON_AddNumberToObject(dl, DBCTL_UNIT_STRING,
            DBCTL_AMBR_UNIT_DEFAULT)) goto failed;

    ul = cJSON_AddObjectToObject(ambr, DBCTL_UPLINK_STRING);
    if (!ul) goto failed;
    if (!cJSON_AddNumberToObject(ul, DBCTL_VALUE_STRING,
            DBCTL_AMBR_VALUE_DEFAULT)) goto failed;
    if (!cJSON_AddNumberToObject(ul, DBCTL_UNIT_STRING,
            DBCTL_AMBR_UNIT_DEFAULT)) goto failed;

    return ambr;

failed:
    cJSON_Delete(ambr);
    return NULL;
}

cJSON *dbctl_build_subscriber(const dbctl_add_args_t *a)
{
    cJSON *doc = NULL, *security, *slice, *slice0, *session, *session0;
    cJSON *qos, *arp, *ambr, *msisdn;
    const char *apn;
    int sst;

    if (!a || !a->imsi || !a->k)
        return NULL;

    doc = cJSON_CreateObject();
    if (!doc)
        return NULL;

    /* schema_version */
    if (!cJSON_AddNumberToObject(doc, DBCTL_SCHEMA_VERSION_STRING,
            DBCTL_SCHEMA_VERSION_DEFAULT)) goto failed;

    /* imsi */
    if (!cJSON_AddStringToObject(doc, DBCTL_IMSI_STRING, a->imsi))
        goto failed;

    /* security { k, opc|op, amf, [rand], sqn } */
    security = cJSON_AddObjectToObject(doc, DBCTL_SECURITY_STRING);
    if (!security) goto failed;

    if (!cJSON_AddStringToObject(security, DBCTL_K_STRING, a->k))
        goto failed;

    if (a->use_opc) {
        if (!a->opc) goto failed;
        if (!cJSON_AddStringToObject(security, DBCTL_OPC_STRING, a->opc))
            goto failed;
    } else {
        if (!a->op) goto failed;
        if (!cJSON_AddStringToObject(security, DBCTL_OP_STRING, a->op))
            goto failed;
    }

    if (!cJSON_AddStringToObject(security, DBCTL_AMF_STRING,
            a->amf ? a->amf : DBCTL_DEFAULT_AMF)) goto failed;

    if (a->rand && a->rand[0]) {
        if (!cJSON_AddStringToObject(security, DBCTL_RAND_STRING, a->rand))
            goto failed;
    }

    if (!cJSON_AddNumberToObject(security, DBCTL_SQN_STRING, 0))
        goto failed;

    /* msisdn[] (optional) */
    if (a->msisdn && a->msisdn[0]) {
        cJSON *m;
        msisdn = cJSON_AddArrayToObject(doc, DBCTL_MSISDN_STRING);
        if (!msisdn) goto failed;
        m = cJSON_CreateString(a->msisdn);
        if (!m) goto failed;
        cJSON_AddItemToArray(msisdn, m);
    }

    /* slice[0] { sst, [sd], default_indicator, session[0] {...} } */
    slice = cJSON_AddArrayToObject(doc, DBCTL_SLICE_STRING);
    if (!slice) goto failed;

    slice0 = cJSON_CreateObject();
    if (!slice0) goto failed;
    cJSON_AddItemToArray(slice, slice0);

    sst = a->sst > 0 ? a->sst : DBCTL_DEFAULT_SST;
    if (!cJSON_AddNumberToObject(slice0, DBCTL_SST_STRING, sst))
        goto failed;

    if (a->has_sd && a->sd && a->sd[0]) {
        if (!cJSON_AddStringToObject(slice0, DBCTL_SD_STRING, a->sd))
            goto failed;
    }

    if (!cJSON_AddBoolToObject(slice0, DBCTL_DEFAULT_INDICATOR_STRING, 1))
        goto failed;

    session = cJSON_AddArrayToObject(slice0, DBCTL_SESSION_STRING);
    if (!session) goto failed;

    session0 = cJSON_CreateObject();
    if (!session0) goto failed;
    cJSON_AddItemToArray(session, session0);

    apn = (a->apn && a->apn[0]) ? a->apn : DBCTL_DEFAULT_APN;
    if (!cJSON_AddStringToObject(session0, DBCTL_NAME_STRING, apn))
        goto failed;

    if (!cJSON_AddNumberToObject(session0, DBCTL_TYPE_STRING,
            DBCTL_SESSION_TYPE_IPV4V6)) goto failed;

    /* session[0].qos { index, arp { ... } } */
    qos = cJSON_AddObjectToObject(session0, DBCTL_QOS_STRING);
    if (!qos) goto failed;
    if (!cJSON_AddNumberToObject(qos, DBCTL_INDEX_STRING,
            DBCTL_QOS_INDEX_DEFAULT)) goto failed;

    arp = cJSON_AddObjectToObject(qos, DBCTL_ARP_STRING);
    if (!arp) goto failed;
    if (!cJSON_AddNumberToObject(arp, DBCTL_PRIORITY_LEVEL_STRING,
            DBCTL_ARP_PRIORITY_LEVEL)) goto failed;
    if (!cJSON_AddNumberToObject(arp, DBCTL_PRE_EMPTION_CAPABILITY_STRING,
            DBCTL_ARP_PRE_EMPTION_CAP)) goto failed;
    if (!cJSON_AddNumberToObject(arp, DBCTL_PRE_EMPTION_VULNERABILITY_STRING,
            DBCTL_ARP_PRE_EMPTION_VUL)) goto failed;

    /* session[0].ambr */
    ambr = build_ambr();
    if (!ambr) goto failed;
    cJSON_AddItemToObject(session0, DBCTL_AMBR_STRING, ambr);

    /* top-level subscriber defaults */
    if (!cJSON_AddNumberToObject(doc, DBCTL_ACCESS_RESTRICTION_DATA_STRING,
            DBCTL_ACCESS_RESTRICTION_DEFAULT)) goto failed;
    if (!cJSON_AddNumberToObject(doc, DBCTL_SUBSCRIBER_STATUS_STRING, 0))
        goto failed;
    if (!cJSON_AddNumberToObject(doc,
            DBCTL_OPERATOR_DETERMINED_BARRING_STRING, 0)) goto failed;
    if (!cJSON_AddNumberToObject(doc, DBCTL_NETWORK_ACCESS_MODE_STRING, 0))
        goto failed;
    if (!cJSON_AddNumberToObject(doc, DBCTL_SUBSCRIBED_RAU_TAU_TIMER_STRING,
            12)) goto failed;

    return doc;

failed:
    cJSON_Delete(doc);
    return NULL;
}

const char *dbctl_subscriber_imsi(const cJSON *doc)
{
    cJSON *item;

    if (!doc)
        return NULL;

    item = cJSON_GetObjectItemCaseSensitive((cJSON *)doc, DBCTL_IMSI_STRING);
    if (!item || !cJSON_IsString(item))
        return NULL;

    return item->valuestring;
}

char *dbctl_build_change_payload(
        const char *imsi, const char *const *fields, int nfields)
{
    cJSON *doc;
    char *out;

    if (!imsi)
        return NULL;

    doc = cJSON_CreateObject();
    if (!doc)
        return NULL;

    if (!cJSON_AddStringToObject(doc, DBCTL_IMSI_STRING, imsi)) {
        cJSON_Delete(doc);
        return NULL;
    }

    /*
     * Omit "fields" entirely when no specific fields are given: the Phase-3
     * watcher treats a missing "fields" list as a full (ALL-fields) refresh.
     */
    if (fields && nfields > 0) {
        cJSON *arr;
        int i;

        arr = cJSON_AddArrayToObject(doc, "fields");
        if (!arr) {
            cJSON_Delete(doc);
            return NULL;
        }
        for (i = 0; i < nfields; i++) {
            cJSON *s;
            if (!fields[i])
                continue;
            s = cJSON_CreateString(fields[i]);
            if (!s) {
                cJSON_Delete(doc);
                return NULL;
            }
            cJSON_AddItemToArray(arr, s);
        }
    }

    out = cJSON_PrintUnformatted(doc);
    cJSON_Delete(doc);
    return out;
}

/*
 * If `node` is an Extended-JSON wrapper object (an object with EXACTLY ONE
 * member whose key is a recognized "$..." marker), build and return the
 * unwrapped primitive (a fresh cJSON the caller owns). Otherwise return NULL.
 */
static cJSON *unwrap_extended_json(const cJSON *node)
{
    cJSON *child;
    const char *key;

    if (!node || !cJSON_IsObject((cJSON *)node))
        return NULL;

    child = node->child;
    if (!child || child->next)     /* must have exactly one member */
        return NULL;

    key = child->string;
    if (!key)
        return NULL;

    if (!strcmp(key, "$numberLong") || !strcmp(key, "$numberInt")) {
        /* Value is typically a string ("96"); occasionally a JSON number. */
        if (cJSON_IsString(child) && child->valuestring) {
            long long v = strtoll(child->valuestring, NULL, 10);
            return cJSON_CreateNumber((double)v);
        }
        if (cJSON_IsNumber(child))
            return cJSON_CreateNumber(child->valuedouble);
        return NULL;
    }

    if (!strcmp(key, "$oid")) {
        if (cJSON_IsString(child) && child->valuestring)
            return cJSON_CreateString(child->valuestring);
        return NULL;
    }

    if (!strcmp(key, "$date")) {
        /* {"$date":<ms>} or {"$date":{"$numberLong":"<ms>"}} -> epoch-ms. */
        if (cJSON_IsNumber(child))
            return cJSON_CreateNumber(child->valuedouble);
        if (cJSON_IsString(child) && child->valuestring) {
            /* Numeric-looking string -> number; else keep as a string. */
            char *end = NULL;
            long long v = strtoll(child->valuestring, &end, 10);
            if (end && *end == '\0' && end != child->valuestring)
                return cJSON_CreateNumber((double)v);
            return cJSON_CreateString(child->valuestring);
        }
        /* {"$date":{"$numberLong":"..."}} : recurse to unwrap the inner first. */
        if (cJSON_IsObject(child)) {
            cJSON *inner = unwrap_extended_json(child);
            if (inner)
                return inner;
        }
        return NULL;
    }

    return NULL;
}

void dbctl_canonicalize_extended_json(cJSON *node)
{
    if (!node)
        return;

    if (cJSON_IsObject((cJSON *)node)) {
        cJSON *child, *next;

        /*
         * Walk the member chain. We may replace a child in place, so capture
         * `next` (and the key) BEFORE replacing: cJSON_ReplaceItemInObject
         * Case Sensitive frees the old child, which would dangle our cursor.
         * The replacement primitive is a leaf, so there is nothing to recurse
         * into afterwards; for non-wrapper children we recurse, then advance.
         */
        for (child = node->child; child != NULL; child = next) {
            cJSON *unwrapped;

            next = child->next;

            unwrapped = unwrap_extended_json(child);
            if (unwrapped) {
                /* child->string is the member key; replace by that key. */
                cJSON_ReplaceItemInObjectCaseSensitive(
                        node, child->string, unwrapped);
                continue;
            }

            /* Not a wrapper: recurse into nested objects/arrays. */
            if (cJSON_IsObject(child) || cJSON_IsArray(child))
                dbctl_canonicalize_extended_json(child);
        }
        return;
    }

    if (cJSON_IsArray((cJSON *)node)) {
        int i, n = cJSON_GetArraySize(node);

        /*
         * Index-based iteration: cJSON_ReplaceItemInArray frees the old element
         * and splices the new one at the same index, so the size and ordering
         * are preserved and `i` stays valid.
         */
        for (i = 0; i < n; i++) {
            cJSON *elem = cJSON_GetArrayItem(node, i);
            cJSON *unwrapped = unwrap_extended_json(elem);

            if (unwrapped) {
                cJSON_ReplaceItemInArray(node, i, unwrapped);
                continue;
            }
            if (cJSON_IsObject(elem) || cJSON_IsArray(elem))
                dbctl_canonicalize_extended_json(elem);
        }
        return;
    }

    /* Scalars: nothing to do. */
}
