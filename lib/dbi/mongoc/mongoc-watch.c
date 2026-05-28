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

#include "mongoc-internal.h"

int mongoc_watch_init(void)
{
#if MONGOC_CHECK_VERSION(1, 9, 0)
    bson_t empty = BSON_INITIALIZER;
    const bson_t *err_doc;
    bson_error_t error;
    bson_t *options = BCON_NEW("fullDocument", "updateLookup");

    ogs_mongoc()->stream = mongoc_collection_watch(
        ogs_mongoc()->collection.subscriber, &empty, options);

    if (mongoc_change_stream_error_document(
            ogs_mongoc()->stream, &error, &err_doc)) {
        if (!bson_empty(err_doc)) {
            ogs_error("Change Stream Error. Enable replica sets to enable "
                      "database updates to be sent to MME.");
        } else {
            ogs_error("Client Error: %s\n", error.message);
        }
        return OGS_ERROR;
    }
    ogs_info("MongoDB Change Streams are Enabled.");
    return OGS_OK;
#else
    return OGS_ERROR;
#endif
}

/*
 * Walk a MongoDB change-stream document into a backend-neutral
 * ogs_dbi_change_event_t. Returns NULL if there is no IMSI (nothing to do).
 * The field-name -> OGS_DBI_FIELD_* mapping below must stay in sync with
 * the consumer in src/hss/hss-context.c:hss_handle_change_event().
 *
 * Behavior parity note: when "updateDescription" is absent (e.g. an insert
 * or full-document replace), the original HSS code set no flags and sent
 * nothing, so we produce mask = 0 here (NOT OGS_DBI_FIELD_ALL).
 */
static ogs_dbi_change_event_t *mongoc_build_change_event(
        const bson_t *document)
{
    bson_iter_t iter, child1_iter, child2_iter;
    char *imsi_bcd = NULL;
    uint32_t mask = 0;
    const char *utf8;
    uint32_t length = 0;
    ogs_dbi_change_event_t *event;

    if (!bson_iter_init_find(&iter, document, "fullDocument"))
        return NULL;

    bson_iter_recurse(&iter, &child1_iter);
    while (bson_iter_next(&child1_iter)) {
        const char *key = bson_iter_key(&child1_iter);
        if (!strcmp(key, "imsi") && BSON_ITER_HOLDS_UTF8(&child1_iter)) {
            utf8 = bson_iter_utf8(&child1_iter, &length);
            imsi_bcd = ogs_strndup(utf8,
                ogs_min(length, OGS_MAX_IMSI_BCD_LEN) + 1);
            ogs_assert(imsi_bcd);
        }
    }

    if (!imsi_bcd)
        return NULL;

    if (bson_iter_init_find(&iter, document, "updateDescription")) {
        bson_iter_recurse(&iter, &child1_iter);
        while (bson_iter_next(&child1_iter)) {
            if (strcmp(bson_iter_key(&child1_iter), "updatedFields") != 0 ||
                    !BSON_ITER_HOLDS_DOCUMENT(&child1_iter))
                continue;

            bson_iter_recurse(&child1_iter, &child2_iter);
            while (bson_iter_next(&child2_iter)) {
                const char *k = bson_iter_key(&child2_iter);

                if (!strcmp(k, "request_cancel_location") &&
                        BSON_ITER_HOLDS_BOOL(&child2_iter) &&
                        bson_iter_bool(&child2_iter)) {
                    mask |= OGS_DBI_FIELD_REQUEST_CANCEL_LOCATION;
                } else if (!strncmp(k, OGS_ACCESS_RESTRICTION_DATA_STRING,
                            strlen(OGS_ACCESS_RESTRICTION_DATA_STRING))) {
                    mask |= OGS_DBI_FIELD_ACCESS_RESTRICTION_DATA;
                } else if (!strncmp(k, OGS_SUBSCRIBER_STATUS_STRING,
                            strlen(OGS_SUBSCRIBER_STATUS_STRING))) {
                    mask |= OGS_DBI_FIELD_SUBSCRIBER_STATUS;
                } else if (!strncmp(k,
                            OGS_OPERATOR_DETERMINED_BARRING_STRING,
                            strlen(OGS_OPERATOR_DETERMINED_BARRING_STRING))) {
                    mask |= OGS_DBI_FIELD_OP_DETERMINED_BARRING;
                } else if (!strncmp(k, OGS_NETWORK_ACCESS_MODE_STRING,
                            strlen(OGS_NETWORK_ACCESS_MODE_STRING))) {
                    mask |= OGS_DBI_FIELD_NETWORK_ACCESS_MODE;
                } else if (!strncmp(k, "ambr", strlen("ambr"))) {
                    mask |= OGS_DBI_FIELD_AMBR;
                } else if (!strncmp(k, OGS_SUBSCRIBED_RAU_TAU_TIMER_STRING,
                            strlen(OGS_SUBSCRIBED_RAU_TAU_TIMER_STRING))) {
                    mask |= OGS_DBI_FIELD_RAU_TAU_TIMER;
                } else if (!strncmp(k, "slice", strlen("slice"))) {
                    mask |= OGS_DBI_FIELD_SLICE;
                }
            }
        }
    }

    event = ogs_dbi_change_event_alloc(imsi_bcd, mask);
    ogs_free(imsi_bcd);
    return event;
}

int mongoc_poll_change_stream(void)
{
#if MONGOC_CHECK_VERSION(1, 9, 0)
    const bson_t *document;
    const bson_t *err_document;
    bson_error_t error;

    if (!ogs_mongoc()->stream)
        return OGS_ERROR;

    while (mongoc_change_stream_next(ogs_mongoc()->stream, &document)) {
        ogs_dbi_change_event_t *ev = mongoc_build_change_event(document);
        if (ev)
            ogs_dbi_dispatch_change_event(ev);
    }

    if (mongoc_change_stream_error_document(
            ogs_mongoc()->stream, &error, &err_document)) {
        if (!bson_empty(err_document)) {
            ogs_debug("Server Error: %s\n",
                bson_as_relaxed_extended_json(err_document, NULL));
        } else {
            ogs_debug("Client Error: %s\n", error.message);
        }
        return OGS_ERROR;
    }
    return OGS_OK;
#else
    return OGS_ERROR;
#endif
}
