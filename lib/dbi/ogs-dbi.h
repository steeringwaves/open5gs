/*
 * Copyright (C) 2019-2023 by Sukchan Lee <acetcom@gmail.com>
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

#ifndef OGS_DBI_H
#define OGS_DBI_H

#include "crypt/ogs-crypt.h"
#include "app/ogs-app.h"

#define OGS_DBI_INSIDE

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Backend-neutral change-event payload, produced by the active backend's
 * change-notification source (Mongo change streams or Redis keyspace
 * events) and consumed by HSS. Each OGS_DBI_FIELD_* bit must stay in sync
 * with the fields HSS examines in hss_handle_change_event(); if HSS adds
 * a new field, add a bit here and update both backend producers.
 */
typedef enum {
    OGS_DBI_FIELD_REQUEST_CANCEL_LOCATION = 1u << 0,
    OGS_DBI_FIELD_ACCESS_RESTRICTION_DATA = 1u << 1,
    OGS_DBI_FIELD_SUBSCRIBER_STATUS       = 1u << 2,
    OGS_DBI_FIELD_OP_DETERMINED_BARRING   = 1u << 3,
    OGS_DBI_FIELD_NETWORK_ACCESS_MODE     = 1u << 4,
    OGS_DBI_FIELD_AMBR                    = 1u << 5,
    OGS_DBI_FIELD_RAU_TAU_TIMER           = 1u << 6,
    OGS_DBI_FIELD_SLICE                   = 1u << 7,
    OGS_DBI_FIELD_ALL                     = 0xFFFFFFFFu,
} ogs_dbi_field_e;

typedef struct ogs_dbi_change_event_s {
    char     *imsi_bcd;            /* heap-allocated via ogs_strdup */
    uint32_t  updated_fields_mask; /* bitmask of ogs_dbi_field_e */
} ogs_dbi_change_event_t;

ogs_dbi_change_event_t *ogs_dbi_change_event_alloc(
        const char *imsi_bcd, uint32_t updated_fields_mask);
void ogs_dbi_change_event_free(ogs_dbi_change_event_t *e);

/*
 * Change-event delivery. A consumer (e.g. HSS) registers a handler; the
 * active backend's poll loop produces ogs_dbi_change_event_t records and
 * the dbi core hands each to the handler. The handler takes ownership of
 * the event and must free it (via ogs_dbi_change_event_free) when done.
 */
typedef void (*ogs_dbi_change_handler_f)(
        ogs_dbi_change_event_t *event, void *data);
void ogs_dbi_set_change_handler(ogs_dbi_change_handler_f handler, void *data);

#ifdef __cplusplus
}
#endif

#ifdef OGS_DBI_HAVE_MONGOC
#include "dbi/ogs-mongoc.h"
#endif
#include "dbi/subscription.h"
#include "dbi/session.h"
#include "dbi/ims.h"

#undef OGS_DBI_INSIDE

#ifdef __cplusplus
extern "C" {
#endif

extern int __ogs_dbi_domain;

int ogs_dbi_init(const char *db_uri);
void ogs_dbi_final(void);

int ogs_dbi_collection_watch_init(void);
int ogs_dbi_poll_change_stream(void);

#undef OGS_LOG_DOMAIN
#define OGS_LOG_DOMAIN __ogs_dbi_domain

#ifdef __cplusplus
}
#endif

#endif /* OGS_DBI_H */
