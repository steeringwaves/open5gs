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

#include "redis/redis-internal.h"

/*
 * AMBR bitrate with unit-scaling. Mirrors mongoc_session_data:
 *   value field is the mantissa; unit is the number of 1000x multiplications
 *   (bps -> kbps -> Mbps -> Gbps ...). Result is stored in bits per second.
 */
static void redis_parse_bitrate(const cJSON *obj, uint64_t *out)
{
    cJSON *v, *u;
    uint8_t unit = 0;
    int n;

    *out = 0;
    if (!obj || !cJSON_IsObject(obj))
        return;

    v = cJSON_GetObjectItemCaseSensitive((cJSON *)obj, OGS_VALUE_STRING);
    u = cJSON_GetObjectItemCaseSensitive((cJSON *)obj, OGS_UNIT_STRING);

    if (v && cJSON_IsNumber(v))
        *out = (uint64_t)cJSON_GetNumberValue(v);
    if (u && cJSON_IsNumber(u))
        unit = (uint8_t)cJSON_GetNumberValue(u);

    for (n = 0; n < unit; n++)
        *out *= 1000;
}

/*
 * Fill one ogs_session_t from a session object (the innermost element of
 * slice[].session[]). Mirrors the per-session walk of mongoc_session_data
 * (name/type/lbo_roaming_allowed/qos/ambr/smf/ue/framed-routes). Kept local to
 * this file because the equivalent helper in redis-subscription.c is static.
 */
static void redis_parse_session(const cJSON *session_obj, ogs_session_t *session)
{
    cJSON *item;
    int rv;

    cJSON_ArrayForEach(item, (cJSON *)session_obj) {
        const char *key = item->string;
        if (!key)
            continue;

        if (!strcmp(key, OGS_NAME_STRING) &&
                cJSON_IsString(item) && item->valuestring) {
            if (session->name)
                ogs_free(session->name);
            session->name = ogs_strdup(item->valuestring);
            ogs_assert(session->name);
        } else if (!strcmp(key, OGS_TYPE_STRING) && cJSON_IsNumber(item)) {
            session->session_type = (uint8_t)cJSON_GetNumberValue(item);
        } else if (!strcmp(key, OGS_LBO_ROAMING_ALLOWED_STRING) &&
                cJSON_IsBool(item)) {
            session->lbo_roaming_allowed = cJSON_IsTrue(item);
        } else if (!strcmp(key, OGS_QOS_STRING) && cJSON_IsObject(item)) {
            cJSON *qos_item;
            cJSON_ArrayForEach(qos_item, item) {
                const char *qos_key = qos_item->string;
                if (!qos_key)
                    continue;
                if (!strcmp(qos_key, OGS_INDEX_STRING) &&
                        cJSON_IsNumber(qos_item)) {
                    session->qos.index =
                        (uint8_t)cJSON_GetNumberValue(qos_item);
                } else if (!strcmp(qos_key, OGS_ARP_STRING) &&
                        cJSON_IsObject(qos_item)) {
                    cJSON *arp_item;
                    cJSON_ArrayForEach(arp_item, qos_item) {
                        const char *arp_key = arp_item->string;
                        if (!arp_key)
                            continue;
                        if (!strcmp(arp_key, OGS_PRIORITY_LEVEL_STRING) &&
                                cJSON_IsNumber(arp_item)) {
                            session->qos.arp.priority_level =
                                (uint8_t)cJSON_GetNumberValue(arp_item);
                        } else if (!strcmp(arp_key,
                                    OGS_PRE_EMPTION_CAPABILITY_STRING) &&
                                cJSON_IsNumber(arp_item)) {
                            session->qos.arp.pre_emption_capability =
                                (uint8_t)cJSON_GetNumberValue(arp_item);
                        } else if (!strcmp(arp_key,
                                    OGS_PRE_EMPTION_VULNERABILITY_STRING) &&
                                cJSON_IsNumber(arp_item)) {
                            session->qos.arp.pre_emption_vulnerability =
                                (uint8_t)cJSON_GetNumberValue(arp_item);
                        }
                    }
                }
            }
        } else if (!strcmp(key, OGS_AMBR_STRING) && cJSON_IsObject(item)) {
            cJSON *dl = cJSON_GetObjectItemCaseSensitive(
                    item, OGS_DOWNLINK_STRING);
            cJSON *ul = cJSON_GetObjectItemCaseSensitive(
                    item, OGS_UPLINK_STRING);
            if (dl && cJSON_IsObject(dl))
                redis_parse_bitrate(dl, &session->ambr.downlink);
            if (ul && cJSON_IsObject(ul))
                redis_parse_bitrate(ul, &session->ambr.uplink);
        } else if (!strcmp(key, OGS_SMF_STRING) && cJSON_IsObject(item)) {
            cJSON *ip_item;
            cJSON_ArrayForEach(ip_item, item) {
                const char *ip_key = ip_item->string;
                if (!ip_key || !cJSON_IsString(ip_item) || !ip_item->valuestring)
                    continue;
                if (!strcmp(ip_key, OGS_IPV4_STRING)) {
                    ogs_ipsubnet_t ipsub;
                    rv = ogs_ipsubnet(&ipsub, ip_item->valuestring, NULL);
                    if (rv == OGS_OK) {
                        session->smf_ip.ipv4 = 1;
                        session->smf_ip.addr = ipsub.sub[0];
                    }
                } else if (!strcmp(ip_key, OGS_IPV6_STRING)) {
                    ogs_ipsubnet_t ipsub;
                    rv = ogs_ipsubnet(&ipsub, ip_item->valuestring, NULL);
                    if (rv == OGS_OK) {
                        session->smf_ip.ipv6 = 1;
                        memcpy(session->smf_ip.addr6, ipsub.sub,
                                sizeof(ipsub.sub));
                    }
                }
            }
        } else if (!strcmp(key, OGS_UE_STRING) && cJSON_IsObject(item)) {
            cJSON *ip_item;
            cJSON_ArrayForEach(ip_item, item) {
                const char *ip_key = ip_item->string;
                if (!ip_key || !cJSON_IsString(ip_item) || !ip_item->valuestring)
                    continue;
                if (!strcmp(ip_key, OGS_IPV4_STRING)) {
                    ogs_ipsubnet_t ipsub;
                    rv = ogs_ipsubnet(&ipsub, ip_item->valuestring, NULL);
                    if (rv == OGS_OK) {
                        session->ue_ip.ipv4 = true;
                        session->ue_ip.addr = ipsub.sub[0];
                    }
                } else if (!strcmp(ip_key, OGS_IPV6_STRING)) {
                    ogs_ipsubnet_t ipsub;
                    rv = ogs_ipsubnet(&ipsub, ip_item->valuestring, NULL);
                    if (rv == OGS_OK) {
                        session->ue_ip.ipv6 = true;
                        memcpy(session->ue_ip.addr6, ipsub.sub, OGS_IPV6_LEN);
                    }
                }
            }
        } else if (!strcmp(key, OGS_IPV4_FRAMED_ROUTES_STRING) &&
                cJSON_IsArray(item)) {
            cJSON *route;
            int i;

            if (session->ipv4_framed_routes) {
                for (i = 0; i < OGS_MAX_NUM_OF_FRAMED_ROUTES_IN_PDI; i++) {
                    if (!session->ipv4_framed_routes[i])
                        break;
                    ogs_free(session->ipv4_framed_routes[i]);
                }
            } else {
                session->ipv4_framed_routes = ogs_calloc(
                        OGS_MAX_NUM_OF_FRAMED_ROUTES_IN_PDI,
                        sizeof(session->ipv4_framed_routes[0]));
            }
            i = 0;
            cJSON_ArrayForEach(route, item) {
                if (i >= OGS_MAX_NUM_OF_FRAMED_ROUTES_IN_PDI)
                    break;
                if (!cJSON_IsString(route) || !route->valuestring)
                    continue;
                session->ipv4_framed_routes[i++] =
                    ogs_strdup(route->valuestring);
            }
        } else if (!strcmp(key, OGS_IPV6_FRAMED_ROUTES_STRING) &&
                cJSON_IsArray(item)) {
            cJSON *route;
            int i;

            if (session->ipv6_framed_routes) {
                for (i = 0; i < OGS_MAX_NUM_OF_FRAMED_ROUTES_IN_PDI; i++) {
                    if (!session->ipv6_framed_routes[i])
                        break;
                    ogs_free(session->ipv6_framed_routes[i]);
                }
            } else {
                session->ipv6_framed_routes = ogs_calloc(
                        OGS_MAX_NUM_OF_FRAMED_ROUTES_IN_PDI,
                        sizeof(session->ipv6_framed_routes[0]));
            }
            i = 0;
            cJSON_ArrayForEach(route, item) {
                if (i >= OGS_MAX_NUM_OF_FRAMED_ROUTES_IN_PDI)
                    break;
                if (!cJSON_IsString(route) || !route->valuestring)
                    continue;
                session->ipv6_framed_routes[i++] =
                    ogs_strdup(route->valuestring);
            }
        }
    }
}

/*
 * Find and fill the ONE session matching S_NSSAI (sst, and sd when present on
 * both sides) AND DNN (== session name). 1:1 port of mongoc_session_data's
 * selection logic. Returns OGS_ERROR if no slice/session matches (mongoc's
 * `found` flag).
 */
int redis_parse_session_data(const cJSON *doc, const ogs_s_nssai_t *s_nssai,
        const char *dnn, ogs_session_data_t *session_data)
{
    cJSON *slice_arr, *slice_el;
    cJSON *matched_session = NULL;
    bool found = false;
    ogs_session_t *session;

    ogs_assert(doc);
    ogs_assert(dnn);
    ogs_assert(session_data);

    slice_arr = cJSON_GetObjectItemCaseSensitive((cJSON *)doc, OGS_SLICE_STRING);
    if (!slice_arr || !cJSON_IsArray(slice_arr)) {
        ogs_error("No '" OGS_SLICE_STRING "' in subscriber doc");
        return OGS_ERROR;
    }

    cJSON_ArrayForEach(slice_el, slice_arr) {
        bool sst_presence = false;
        uint8_t sst = 0;
        ogs_uint24_t sd;
        cJSON *slice_item, *session_arr = NULL;

        sd.v = OGS_S_NSSAI_NO_SD_VALUE;

        if (!cJSON_IsObject(slice_el))
            continue;

        cJSON_ArrayForEach(slice_item, slice_el) {
            const char *slice_key = slice_item->string;
            if (!slice_key)
                continue;

            if (!strcmp(slice_key, OGS_SST_STRING) &&
                    cJSON_IsNumber(slice_item)) {
                sst_presence = true;
                sst = (uint8_t)cJSON_GetNumberValue(slice_item);
            } else if (!strcmp(slice_key, OGS_SD_STRING) &&
                    cJSON_IsString(slice_item) && slice_item->valuestring) {
                sd = ogs_s_nssai_sd_from_string(slice_item->valuestring);
            } else if (!strcmp(slice_key, OGS_SESSION_STRING) &&
                    cJSON_IsArray(slice_item)) {
                session_arr = slice_item;
            }
        }

        if (!sst_presence) {
            ogs_error("No SST");
            continue;
        }

        if (s_nssai && s_nssai->sst != sst)
            continue;

        if (s_nssai &&
                s_nssai->sd.v != OGS_S_NSSAI_NO_SD_VALUE &&
                sd.v != OGS_S_NSSAI_NO_SD_VALUE) {
            if (s_nssai->sd.v != sd.v)
                continue;
        }

        if (!session_arr)
            continue;

        {
            cJSON *session_el;
            cJSON_ArrayForEach(session_el, session_arr) {
                cJSON *name = cJSON_GetObjectItemCaseSensitive(
                        session_el, OGS_NAME_STRING);
                if (name && cJSON_IsString(name) && name->valuestring &&
                        ogs_strcasecmp(name->valuestring, dnn) == 0) {
                    matched_session = session_el;
                    found = true;
                    break;
                }
            }
        }

        if (found)
            break;
    }

    if (!found) {
        ogs_error("Cannot find S_NSSAI[SST:%d SD:0x%x] DNN[%s] in DB",
                s_nssai ? s_nssai->sst : 0,
                s_nssai ? s_nssai->sd.v : 0,
                dnn);
        return OGS_ERROR;
    }

    session = &session_data->session;
    redis_parse_session(matched_session, session);

    return OGS_OK;
}

int redis_session_data(const char *supi, const ogs_s_nssai_t *s_nssai,
        const char *dnn, ogs_session_data_t *session_data)
{
    char *key, *json;
    cJSON *doc;
    int rv;

    ogs_assert(supi); ogs_assert(dnn); ogs_assert(session_data);
    key = redis_subscriber_key(supi);
    if (!key) return OGS_ERROR;
    json = redis_get_string(key);
    ogs_free(key);
    if (!json) { ogs_error("[%s] Cannot find IMSI in DB", supi); return OGS_ERROR; }
    doc = cJSON_Parse(json);
    ogs_free(json);
    if (!doc) { ogs_error("[%s] malformed subscriber JSON", supi); return OGS_ERROR; }
    rv = redis_parse_session_data(doc, s_nssai, dnn, session_data);
    cJSON_Delete(doc);
    return rv;
}
