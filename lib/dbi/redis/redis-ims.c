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
 * Parse `imsi` + `msisdn[]` into MSISDN_DATA. 1:1 port of the BSON walker in
 * mongoc_msisdn_data: imsi -> imsi.bcd + ogs_bcd_to_buffer; each msisdn[] entry
 * -> msisdn[i].bcd + ogs_bcd_to_buffer.
 */
int redis_parse_msisdn_data(const cJSON *doc, ogs_msisdn_data_t *msisdn_data)
{
    cJSON *item;

    ogs_assert(doc);
    ogs_assert(msisdn_data);

    memset(msisdn_data, 0, sizeof(*msisdn_data));

    cJSON_ArrayForEach(item, (cJSON *)doc) {
        const char *key = item->string;
        if (!key)
            continue;

        if (!strcmp(key, OGS_IMSI_STRING) &&
                cJSON_IsString(item) && item->valuestring) {
            ogs_cpystrn(msisdn_data->imsi.bcd, item->valuestring,
                    ogs_min(strlen(item->valuestring),
                        OGS_MAX_IMSI_BCD_LEN)+1);
            ogs_bcd_to_buffer(
                    msisdn_data->imsi.bcd,
                    msisdn_data->imsi.buf, &msisdn_data->imsi.len);
        } else if (!strcmp(key, OGS_MSISDN_STRING) && cJSON_IsArray(item)) {
            cJSON *msisdn_el;
            int msisdn_index = 0;

            cJSON_ArrayForEach(msisdn_el, item) {
                ogs_assert(msisdn_index < OGS_MAX_NUM_OF_MSISDN);

                if (cJSON_IsString(msisdn_el) && msisdn_el->valuestring) {
                    ogs_cpystrn(msisdn_data->msisdn[msisdn_index].bcd,
                            msisdn_el->valuestring,
                            ogs_min(strlen(msisdn_el->valuestring),
                                OGS_MAX_MSISDN_BCD_LEN)+1);
                    ogs_bcd_to_buffer(
                            msisdn_data->msisdn[msisdn_index].bcd,
                            msisdn_data->msisdn[msisdn_index].buf,
                            &msisdn_data->msisdn[msisdn_index].len);

                    msisdn_index++;
                }
            }
            msisdn_data->num_of_msisdn = msisdn_index;
        }
    }

    return OGS_OK;
}

int redis_msisdn_data(char *id, ogs_msisdn_data_t *msisdn_data)
{
    char *index_key, *imsi, *sub_key, *json;
    cJSON *doc;
    int rv;

    ogs_assert(id);
    ogs_assert(msisdn_data);

    /*
     * Resolve by IMSI OR MSISDN (mongoc used $or [imsi, msisdn]):
     *   1. GET <prefix>msisdn:<id> -> if a string, it's the IMSI value; load
     *      <prefix>subscriber:<imsi>.
     *   2. Else treat `id` as the IMSI/value directly:
     *      <prefix>subscriber:<id>.
     */
    index_key = ogs_msprintf("%smsisdn:%s", ogs_redis()->prefix, id);
    ogs_assert(index_key);
    imsi = redis_get_string(index_key);
    ogs_free(index_key);

    if (imsi) {
        /* The index value is already a bare IMSI value (no type prefix). */
        sub_key = ogs_msprintf("%ssubscriber:%s", ogs_redis()->prefix, imsi);
        ogs_assert(sub_key);
        ogs_free(imsi);
    } else {
        sub_key = redis_subscriber_key(id);
        if (!sub_key)
            return OGS_ERROR;
    }

    json = redis_get_string(sub_key);
    ogs_free(sub_key);
    if (!json) {
        ogs_error("[%s] Cannot find IMSI or MSISDN in DB", id);
        return OGS_ERROR;
    }
    doc = cJSON_Parse(json);
    ogs_free(json);
    if (!doc) {
        ogs_error("[%s] malformed subscriber JSON", id);
        return OGS_ERROR;
    }
    rv = redis_parse_msisdn_data(doc, msisdn_data);
    cJSON_Delete(doc);
    return rv;
}

/*
 * Parse one spt object (the innermost element of trigger_point.spt[]). 1:1 port
 * of the per-spt walk in mongoc_ims_data: condition_negated, group, and the
 * per-type one-of (method/session_case/sip_header/sdp_line/request_uri) with
 * the matching OGS_SPT_HAS_* type tagging.
 */
static void redis_parse_spt(const cJSON *spt_obj, ogs_spt_t *spt)
{
    cJSON *item;

    cJSON_ArrayForEach(item, (cJSON *)spt_obj) {
        const char *key = item->string;
        if (!key)
            continue;

        if (!strcmp(key, "condition_negated") && cJSON_IsNumber(item)) {
            spt->condition_negated = (int)cJSON_GetNumberValue(item);
        } else if (!strcmp(key, "group") && cJSON_IsNumber(item)) {
            spt->group = (int)cJSON_GetNumberValue(item);
        } else if (!strcmp(key, "method") &&
                cJSON_IsString(item) && item->valuestring) {
            spt->method = ogs_strdup(item->valuestring);
            spt->type = OGS_SPT_HAS_METHOD;
        } else if (!strcmp(key, "session_case") && cJSON_IsNumber(item)) {
            spt->session_case = (int)cJSON_GetNumberValue(item);
            spt->type = OGS_SPT_HAS_SESSION_CASE;
        } else if (!strcmp(key, "sip_header") && cJSON_IsObject(item)) {
            cJSON *sip_item;
            cJSON_ArrayForEach(sip_item, item) {
                const char *sip_key = sip_item->string;
                if (!sip_key)
                    continue;
                if (!strcmp(sip_key, "header") &&
                        cJSON_IsString(sip_item) && sip_item->valuestring) {
                    spt->header = ogs_strdup(sip_item->valuestring);
                } else if (!strcmp(sip_key, "content") &&
                        cJSON_IsString(sip_item) && sip_item->valuestring) {
                    spt->header_content = ogs_strdup(sip_item->valuestring);
                }
            }
            spt->type = OGS_SPT_HAS_SIP_HEADER;
        } else if (!strcmp(key, "sdp_line") && cJSON_IsObject(item)) {
            cJSON *sdp_item;
            cJSON_ArrayForEach(sdp_item, item) {
                const char *sdp_key = sdp_item->string;
                if (!sdp_key)
                    continue;
                if (!strcmp(sdp_key, "line") &&
                        cJSON_IsString(sdp_item) && sdp_item->valuestring) {
                    spt->sdp_line = ogs_strdup(sdp_item->valuestring);
                } else if (!strcmp(sdp_key, "content") &&
                        cJSON_IsString(sdp_item) && sdp_item->valuestring) {
                    spt->sdp_line_content = ogs_strdup(sdp_item->valuestring);
                }
            }
            spt->type = OGS_SPT_HAS_SDP_LINE;
        } else if (!strcmp(key, "request_uri") &&
                cJSON_IsString(item) && item->valuestring) {
            spt->request_uri = ogs_strdup(item->valuestring);
            spt->type = OGS_SPT_HAS_REQUEST_URI;
        }
    }
}

/*
 * Parse one ifc object (element of ifc[]). 1:1 port of the per-ifc walk in
 * mongoc_ims_data: priority, application_server{server_name, default_handling},
 * trigger_point{condition_type_cnf, spt[]}.
 */
static void redis_parse_ifc(const cJSON *ifc_obj, ogs_ifc_t *ifc)
{
    cJSON *item;

    cJSON_ArrayForEach(item, (cJSON *)ifc_obj) {
        const char *key = item->string;
        if (!key)
            continue;

        if (!strcmp(key, "priority") && cJSON_IsNumber(item)) {
            ifc->priority = (int)cJSON_GetNumberValue(item);
        } else if (!strcmp(key, "application_server") && cJSON_IsObject(item)) {
            cJSON *as_item;
            cJSON_ArrayForEach(as_item, item) {
                const char *as_key = as_item->string;
                if (!as_key)
                    continue;
                if (!strcmp(as_key, "server_name") &&
                        cJSON_IsString(as_item) && as_item->valuestring) {
                    ifc->application_server.server_name =
                        ogs_strdup(as_item->valuestring);
                } else if (!strcmp(as_key, "default_handling") &&
                        cJSON_IsNumber(as_item)) {
                    ifc->application_server.default_handling =
                        (int)cJSON_GetNumberValue(as_item);
                }
            }
        } else if (!strcmp(key, "trigger_point") && cJSON_IsObject(item)) {
            cJSON *tp_item;
            cJSON_ArrayForEach(tp_item, item) {
                const char *tp_key = tp_item->string;
                if (!tp_key)
                    continue;
                if (!strcmp(tp_key, "condition_type_cnf") &&
                        cJSON_IsNumber(tp_item)) {
                    ifc->trigger_point.condition_type_cnf =
                        (int)cJSON_GetNumberValue(tp_item);
                } else if (!strcmp(tp_key, "spt") && cJSON_IsArray(tp_item)) {
                    cJSON *spt_el;
                    int spt_index = 0;

                    cJSON_ArrayForEach(spt_el, tp_item) {
                        ogs_assert(spt_index < OGS_MAX_NUM_OF_SPT);
                        if (!cJSON_IsObject(spt_el))
                            continue;
                        redis_parse_spt(spt_el,
                                &ifc->trigger_point.spt[spt_index]);
                        spt_index++;
                    }
                    ifc->trigger_point.num_of_spt = spt_index;
                }
            }
        }
    }
}

/*
 * Parse `msisdn[]` + `ifc[]` into IMS_DATA. 1:1 port of mongoc_ims_data's BSON
 * walker.
 */
int redis_parse_ims_data(const cJSON *doc, ogs_ims_data_t *ims_data)
{
    cJSON *item;

    ogs_assert(doc);
    ogs_assert(ims_data);

    memset(ims_data, 0, sizeof(*ims_data));

    cJSON_ArrayForEach(item, (cJSON *)doc) {
        const char *key = item->string;
        if (!key)
            continue;

        if (!strcmp(key, OGS_MSISDN_STRING) && cJSON_IsArray(item)) {
            cJSON *msisdn_el;
            int msisdn_index = 0;

            cJSON_ArrayForEach(msisdn_el, item) {
                ogs_assert(msisdn_index < OGS_MAX_NUM_OF_MSISDN);

                if (cJSON_IsString(msisdn_el) && msisdn_el->valuestring) {
                    ogs_cpystrn(ims_data->msisdn[msisdn_index].bcd,
                            msisdn_el->valuestring,
                            ogs_min(strlen(msisdn_el->valuestring),
                                OGS_MAX_MSISDN_BCD_LEN)+1);
                    ogs_bcd_to_buffer(
                            ims_data->msisdn[msisdn_index].bcd,
                            ims_data->msisdn[msisdn_index].buf,
                            &ims_data->msisdn[msisdn_index].len);

                    msisdn_index++;
                }
            }
            ims_data->num_of_msisdn = msisdn_index;
        } else if (!strcmp(key, "ifc") && cJSON_IsArray(item)) {
            cJSON *ifc_el;
            int ifc_index = 0;

            cJSON_ArrayForEach(ifc_el, item) {
                ogs_assert(ifc_index < OGS_MAX_NUM_OF_IFC);
                if (!cJSON_IsObject(ifc_el))
                    continue;
                redis_parse_ifc(ifc_el, &ims_data->ifc[ifc_index]);
                ifc_index++;
            }
            ims_data->num_of_ifc = ifc_index;
        }
    }

    return OGS_OK;
}

int redis_ims_data(char *supi, ogs_ims_data_t *ims_data)
{
    char *key, *json;
    cJSON *doc;
    int rv;

    ogs_assert(supi);
    ogs_assert(ims_data);

    key = redis_subscriber_key(supi);
    if (!key) return OGS_ERROR;
    json = redis_get_string(key);
    ogs_free(key);
    if (!json) { ogs_error("[%s] Cannot find IMSI in DB", supi); return OGS_ERROR; }
    doc = cJSON_Parse(json);
    ogs_free(json);
    if (!doc) { ogs_error("[%s] malformed subscriber JSON", supi); return OGS_ERROR; }
    rv = redis_parse_ims_data(doc, ims_data);
    cJSON_Delete(doc);
    return rv;
}
