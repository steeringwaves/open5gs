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

#ifndef DBCTL_SUBSCRIBER_H
#define DBCTL_SUBSCRIBER_H

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Arguments collected from the `add` command's flags. All string fields point
 * into argv (not owned). The builder copies what it needs into the cJSON doc.
 */
typedef struct dbctl_add_args_s {
    const char *imsi;       /* required */
    const char *k;          /* required (hex string, stored as-is) */
    const char *opc;        /* hex string; used when use_opc is set */
    const char *op;         /* hex string; used when use_opc is clear */
    int use_opc;            /* 1 -> emit security.opc, 0 -> security.op */
    const char *amf;        /* hex string; default "8000" if NULL */
    const char *rand;       /* optional hex string */
    const char *apn;        /* session/DNN name; default "internet" if NULL */
    int sst;                /* slice SST; default 1 if not given */
    const char *sd;         /* slice SD hex string; emitted only if has_sd */
    int has_sd;             /* 1 -> emit slice[0].sd */
    const char *msisdn;     /* optional; emitted as a one-element msisdn[] */
} dbctl_add_args_t;

/*
 * Build a subscriber JSON document from `a`, mirroring the webui defaults and
 * the field names/nesting the Phase-2 reader expects (see
 * lib/dbi/redis/redis-subscription.c: redis_parse_auth_info /
 * redis_parse_subscription_data). Returns a new cJSON object (caller frees with
 * cJSON_Delete) or NULL on allocation failure.
 *
 * PURE: no Redis, no I/O.
 */
cJSON *dbctl_build_subscriber(const dbctl_add_args_t *a);

/*
 * Return the top-level "imsi" string of a parsed subscriber doc, or NULL if it
 * is absent / not a string. The returned pointer is owned by `doc`.
 */
const char *dbctl_subscriber_imsi(const cJSON *doc);

/*
 * Build the JSON payload published to "<prefix>events:subscriber" for a
 * change event:  {"imsi":"<imsi>","fields":["ambr","slice",...]}
 *
 * When `fields`/`nfields` describe no fields (fields == NULL or nfields == 0),
 * the "fields" key is OMITTED entirely:  {"imsi":"<imsi>"}. The Phase-3 watcher
 * (lib/dbi/redis/redis-watch.c: redis_parse_rich_event) treats a missing/empty
 * "fields" list as "refresh ALL fields", so this is how a caller requests a
 * full refresh. Field names must be ones the watcher maps via
 * redis_event_field_from_name() (e.g. "ambr", "slice",
 * "subscriber_status", ...).
 *
 * Returns a heap string the caller frees with cJSON_free(), or NULL on error
 * (including a NULL imsi). PURE: no Redis, no I/O.
 */
char *dbctl_build_change_payload(
        const char *imsi, const char *const *fields, int nfields);

#ifdef __cplusplus
}
#endif

#endif /* DBCTL_SUBSCRIBER_H */
