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

uint32_t redis_event_field_from_name(const char *name)
{
    if (!name) return 0;
    if (!strcmp(name, "request_cancel_location"))
        return OGS_DBI_FIELD_REQUEST_CANCEL_LOCATION;
    if (!strncmp(name, OGS_ACCESS_RESTRICTION_DATA_STRING,
            strlen(OGS_ACCESS_RESTRICTION_DATA_STRING)))
        return OGS_DBI_FIELD_ACCESS_RESTRICTION_DATA;
    if (!strncmp(name, OGS_SUBSCRIBER_STATUS_STRING,
            strlen(OGS_SUBSCRIBER_STATUS_STRING)))
        return OGS_DBI_FIELD_SUBSCRIBER_STATUS;
    if (!strncmp(name, OGS_OPERATOR_DETERMINED_BARRING_STRING,
            strlen(OGS_OPERATOR_DETERMINED_BARRING_STRING)))
        return OGS_DBI_FIELD_OP_DETERMINED_BARRING;
    if (!strncmp(name, OGS_NETWORK_ACCESS_MODE_STRING,
            strlen(OGS_NETWORK_ACCESS_MODE_STRING)))
        return OGS_DBI_FIELD_NETWORK_ACCESS_MODE;
    if (!strncmp(name, "ambr", strlen("ambr")))
        return OGS_DBI_FIELD_AMBR;
    if (!strncmp(name, OGS_SUBSCRIBED_RAU_TAU_TIMER_STRING,
            strlen(OGS_SUBSCRIBED_RAU_TAU_TIMER_STRING)))
        return OGS_DBI_FIELD_RAU_TAU_TIMER;
    if (!strncmp(name, "slice", strlen("slice")))
        return OGS_DBI_FIELD_SLICE;
    return 0;
}

int redis_parse_rich_event(const char *json, char **imsi_out, uint32_t *mask_out)
{
    cJSON *doc, *imsi, *fields;
    uint32_t mask = 0;

    ogs_assert(imsi_out); ogs_assert(mask_out);
    *imsi_out = NULL; *mask_out = 0;

    doc = cJSON_Parse(json);
    if (!doc) return OGS_ERROR;

    imsi = cJSON_GetObjectItemCaseSensitive(doc, "imsi");
    if (!imsi || !cJSON_IsString(imsi) || !imsi->valuestring) {
        cJSON_Delete(doc);
        return OGS_ERROR;
    }
    *imsi_out = ogs_strdup(imsi->valuestring);

    fields = cJSON_GetObjectItemCaseSensitive(doc, "fields");
    if (fields && cJSON_IsArray(fields) && cJSON_GetArraySize(fields) > 0) {
        cJSON *f;
        cJSON_ArrayForEach(f, fields) {
            if (cJSON_IsString(f) && f->valuestring)
                mask |= redis_event_field_from_name(f->valuestring);
        }
        if (mask == 0) mask = OGS_DBI_FIELD_ALL; /* unknown fields → refresh all */
    } else {
        mask = OGS_DBI_FIELD_ALL;               /* no field list → refresh all */
    }
    *mask_out = mask;

    cJSON_Delete(doc);
    return OGS_OK;
}

char *redis_keyspace_channel_to_imsi(const char *channel, const char *prefix)
{
    const char *p, *head_end;
    char *head;

    if (!channel || !prefix) return NULL;
    /* skip "__keyspace@<db>__:" → find the ':' that ends that segment */
    p = strchr(channel, ':');
    if (!p) return NULL;
    p++;  /* now at "<prefix>subscriber:<imsi>" */

    head = ogs_msprintf("%ssubscriber:", prefix);
    ogs_assert(head);
    if (strncmp(p, head, strlen(head)) != 0) {
        ogs_free(head);
        return NULL;
    }
    head_end = p + strlen(head);
    ogs_free(head);
    if (*head_end == '\0') return NULL;
    return ogs_strdup(head_end);
}
