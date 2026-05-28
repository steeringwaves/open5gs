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
