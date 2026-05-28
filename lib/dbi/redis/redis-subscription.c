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

int redis_parse_auth_info(const cJSON *doc, ogs_dbi_auth_info_t *auth_info)
{
    cJSON *security, *item;
    char buf[OGS_KEY_LEN];

    ogs_assert(doc); ogs_assert(auth_info);
    memset(auth_info, 0, sizeof(*auth_info));

    security = cJSON_GetObjectItemCaseSensitive((cJSON *)doc, OGS_SECURITY_STRING);
    if (!security || !cJSON_IsObject(security)) {
        ogs_error("No '" OGS_SECURITY_STRING "' in subscriber doc");
        return OGS_ERROR;
    }

    cJSON_ArrayForEach(item, security) {
        const char *key = item->string;
        if (!key)
            continue;
        if (!strcmp(key, OGS_K_STRING) && cJSON_IsString(item)) {
            ogs_ascii_to_hex(item->valuestring, strlen(item->valuestring),
                    buf, sizeof(buf));
            memcpy(auth_info->k, buf, OGS_KEY_LEN);
        } else if (!strcmp(key, OGS_OPC_STRING) && cJSON_IsString(item)) {
            auth_info->use_opc = 1;
            ogs_ascii_to_hex(item->valuestring, strlen(item->valuestring),
                    buf, sizeof(buf));
            memcpy(auth_info->opc, buf, OGS_KEY_LEN);
        } else if (!strcmp(key, OGS_OP_STRING) && cJSON_IsString(item)) {
            ogs_ascii_to_hex(item->valuestring, strlen(item->valuestring),
                    buf, sizeof(buf));
            memcpy(auth_info->op, buf, OGS_KEY_LEN);
        } else if (!strcmp(key, OGS_AMF_STRING) && cJSON_IsString(item)) {
            ogs_ascii_to_hex(item->valuestring, strlen(item->valuestring),
                    buf, sizeof(buf));
            memcpy(auth_info->amf, buf, OGS_AMF_LEN);
        } else if (!strcmp(key, OGS_RAND_STRING) && cJSON_IsString(item)) {
            ogs_ascii_to_hex(item->valuestring, strlen(item->valuestring),
                    buf, sizeof(buf));
            memcpy(auth_info->rand, buf, OGS_RAND_LEN);
        } else if (!strcmp(key, OGS_SQN_STRING) && cJSON_IsNumber(item)) {
            auth_info->sqn = (uint64_t)cJSON_GetNumberValue(item);  /* NOT valueint */
        }
    }
    return OGS_OK;
}

int redis_auth_info(char *supi, ogs_dbi_auth_info_t *auth_info)
{
    char *key, *json;
    cJSON *doc;
    int rv;

    ogs_assert(supi); ogs_assert(auth_info);
    key = redis_subscriber_key(supi);
    if (!key) return OGS_ERROR;
    json = redis_get_string(key);
    ogs_free(key);
    if (!json) { ogs_info("[%s] Cannot find IMSI in DB", supi); return OGS_ERROR; }
    doc = cJSON_Parse(json);
    ogs_free(json);
    if (!doc) { ogs_error("[%s] malformed subscriber JSON", supi); return OGS_ERROR; }
    rv = redis_parse_auth_info(doc, auth_info);
    cJSON_Delete(doc);
    return rv;
}

/*
 * AMBR bitrate with unit-scaling. Mirrors mongoc_subscription_data:
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

/* Parse one session object (the innermost element of slice[].session[]). */
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

int redis_parse_subscription_data(const cJSON *doc,
        ogs_subscription_data_t *subscription_data)
{
    cJSON *item;

    ogs_assert(doc);
    ogs_assert(subscription_data);

    memset(subscription_data, 0, sizeof(*subscription_data));

    cJSON_ArrayForEach(item, (cJSON *)doc) {
        const char *key = item->string;
        if (!key)
            continue;

        if (!strcmp(key, OGS_MSISDN_STRING) && cJSON_IsArray(item)) {
            cJSON *msisdn;
            int msisdn_index = 0;

            cJSON_ArrayForEach(msisdn, item) {
                ogs_assert(msisdn_index < OGS_MAX_NUM_OF_MSISDN);

                if (cJSON_IsString(msisdn) && msisdn->valuestring) {
                    size_t length = strlen(msisdn->valuestring);
                    ogs_cpystrn(subscription_data->msisdn[msisdn_index].bcd,
                            msisdn->valuestring,
                            ogs_min(length, OGS_MAX_MSISDN_BCD_LEN) + 1);
                    ogs_bcd_to_buffer(
                            subscription_data->msisdn[msisdn_index].bcd,
                            subscription_data->msisdn[msisdn_index].buf,
                            &subscription_data->msisdn[msisdn_index].len);

                    msisdn_index++;
                }
            }
            subscription_data->num_of_msisdn = msisdn_index;

        } else if (!strcmp(key, OGS_IMSI_STRING) &&
                cJSON_IsString(item) && item->valuestring) {
            subscription_data->imsi = ogs_strndup(item->valuestring,
                    ogs_min(strlen(item->valuestring), OGS_MAX_IMSI_BCD_LEN) + 1);
            ogs_assert(subscription_data->imsi);
        } else if (!strcmp(key, OGS_ACCESS_RESTRICTION_DATA_STRING) &&
                cJSON_IsNumber(item)) {
            subscription_data->access_restriction_data =
                (uint32_t)cJSON_GetNumberValue(item);
        } else if (!strcmp(key, OGS_SUBSCRIBER_STATUS_STRING) &&
                cJSON_IsNumber(item)) {
            subscription_data->subscriber_status =
                (uint32_t)cJSON_GetNumberValue(item);
        } else if (!strcmp(key, OGS_OPERATOR_DETERMINED_BARRING_STRING) &&
                cJSON_IsNumber(item)) {
            subscription_data->operator_determined_barring =
                (uint32_t)cJSON_GetNumberValue(item);
        } else if (!strcmp(key, OGS_NETWORK_ACCESS_MODE_STRING) &&
                cJSON_IsNumber(item)) {
            subscription_data->network_access_mode =
                (uint32_t)cJSON_GetNumberValue(item);
        } else if (!strcmp(key, OGS_SUBSCRIBED_RAU_TAU_TIMER_STRING) &&
                cJSON_IsNumber(item)) {
            subscription_data->subscribed_rau_tau_timer =
                (uint32_t)cJSON_GetNumberValue(item);
        } else if (!strcmp(key, OGS_AMBR_STRING) && cJSON_IsObject(item)) {
            cJSON *dl = cJSON_GetObjectItemCaseSensitive(
                    item, OGS_DOWNLINK_STRING);
            cJSON *ul = cJSON_GetObjectItemCaseSensitive(
                    item, OGS_UPLINK_STRING);
            if (dl && cJSON_IsObject(dl))
                redis_parse_bitrate(dl, &subscription_data->ambr.downlink);
            if (ul && cJSON_IsObject(ul))
                redis_parse_bitrate(ul, &subscription_data->ambr.uplink);
        } else if (!strcmp(key, OGS_SLICE_STRING) && cJSON_IsArray(item)) {
            cJSON *slice_el;

            cJSON_ArrayForEach(slice_el, item) {
                ogs_slice_data_t *slice_data = NULL;
                bool sst_presence = false;
                cJSON *slice_item;

                ogs_assert(subscription_data->num_of_slice <
                        OGS_MAX_NUM_OF_SLICE);

                slice_data =
                    &subscription_data->slice[subscription_data->num_of_slice];

                slice_data->s_nssai.sst = 0;
                slice_data->s_nssai.sd.v = OGS_S_NSSAI_NO_SD_VALUE;

                cJSON_ArrayForEach(slice_item, slice_el) {
                    const char *slice_key = slice_item->string;
                    if (!slice_key)
                        continue;

                    if (!strcmp(slice_key, OGS_SST_STRING) &&
                            cJSON_IsNumber(slice_item)) {
                        slice_data->s_nssai.sst =
                            (uint8_t)cJSON_GetNumberValue(slice_item);
                        sst_presence = true;
                    } else if (!strcmp(slice_key, OGS_SD_STRING) &&
                            cJSON_IsString(slice_item) &&
                            slice_item->valuestring) {
                        slice_data->s_nssai.sd =
                            ogs_s_nssai_sd_from_string(slice_item->valuestring);
                    } else if (!strcmp(slice_key, OGS_DEFAULT_INDICATOR_STRING) &&
                            cJSON_IsBool(slice_item)) {
                        slice_data->default_indicator = cJSON_IsTrue(slice_item);
                    } else if (!strcmp(slice_key, OGS_SESSION_STRING) &&
                            cJSON_IsArray(slice_item)) {
                        cJSON *session_el;

                        cJSON_ArrayForEach(session_el, slice_item) {
                            ogs_session_t *session = NULL;

                            ogs_assert(slice_data->num_of_session <
                                    OGS_MAX_NUM_OF_SESS);
                            session =
                                &slice_data->session[slice_data->num_of_session];

                            redis_parse_session(session_el, session);

                            slice_data->num_of_session++;
                        }
                    }
                }

                if (!sst_presence) {
                    ogs_error("No SST");
                    continue;
                }

                subscription_data->num_of_slice++;
            }
        } else if (!strcmp(key, OGS_MME_HOST_STRING) &&
                cJSON_IsString(item) && item->valuestring) {
            subscription_data->mme_host = ogs_strndup(item->valuestring,
                    ogs_min(strlen(item->valuestring), OGS_MAX_FQDN_LEN) + 1);
            ogs_assert(subscription_data->mme_host);
        } else if (!strcmp(key, OGS_MME_REALM_STRING) &&
                cJSON_IsString(item) && item->valuestring) {
            subscription_data->mme_realm = ogs_strndup(item->valuestring,
                    ogs_min(strlen(item->valuestring), OGS_MAX_FQDN_LEN) + 1);
            ogs_assert(subscription_data->mme_realm);
        } else if (!strcmp(key, OGS_PURGE_FLAG_STRING) && cJSON_IsBool(item)) {
            subscription_data->purge_flag = cJSON_IsTrue(item);
        }
    }

    return OGS_OK;
}

int redis_subscription_data(char *supi,
        ogs_subscription_data_t *subscription_data)
{
    char *key, *json;
    cJSON *doc;
    int rv;

    ogs_assert(supi); ogs_assert(subscription_data);
    key = redis_subscriber_key(supi);
    if (!key) return OGS_ERROR;
    json = redis_get_string(key);
    ogs_free(key);
    if (!json) { ogs_error("[%s] Cannot find IMSI in DB", supi); return OGS_ERROR; }
    doc = cJSON_Parse(json);
    ogs_free(json);
    if (!doc) { ogs_error("[%s] malformed subscriber JSON", supi); return OGS_ERROR; }
    rv = redis_parse_subscription_data(doc, subscription_data);
    cJSON_Delete(doc);
    return rv;
}
