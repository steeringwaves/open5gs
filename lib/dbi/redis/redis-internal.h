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

#ifndef OGS_DBI_REDIS_INTERNAL_H
#define OGS_DBI_REDIS_INTERNAL_H

#include "ogs-dbi-backend.h"
#include "redis/cJSON.h"

#include <hiredis.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OGS_REDIS_DEFAULT_PREFIX "open5gs:"
#define OGS_REDIS_DEFAULT_PORT 6379
#define OGS_REDIS_WATCH_MAX_RETRY 5

#define OGS_REDIS_SUPPRESS_SLOTS 16
#define OGS_REDIS_SUPPRESS_WINDOW ogs_time_from_msec(200)
#define OGS_REDIS_EVENTS_CHANNEL_SUFFIX "events:subscriber"

typedef struct ogs_redis_s {
    bool initialized;
    char *host;
    int port;
    char *prefix;          /* e.g. "open5gs:" */
    char *masked_uri;
    redisContext *ctx;     /* sync connection for GET/SET/WATCH/MULTI */
    redisContext *sub_ctx; /* dedicated pub/sub connection for watch */
    struct {
        char imsi_bcd[OGS_MAX_IMSI_BCD_LEN + 1];
        ogs_time_t when;
    } suppress[OGS_REDIS_SUPPRESS_SLOTS];
    int suppress_next;     /* ring write index */
} ogs_redis_t;

ogs_redis_t *ogs_redis(void);

extern const ogs_dbi_backend_t redis_backend;

/* Connection lifecycle (redis-backend.c) */
int  redis_init(const char *uri);
void redis_final(void);

/* URI parser. Non-static so unit tests can exercise it directly. */
int  redis_parse_uri(const char *uri, char **host, int *port, char **prefix);

/* Low-level helpers (redis-backend.c) */
/* Returns a heap string (caller ogs_free) for "<prefix>subscriber:<value>". */
char *redis_subscriber_key(const char *supi);
/* GET <key> -> returns malloc'd JSON string (caller ogs_free) or NULL if missing. */
char *redis_get_string(const char *key);
/*
 * Atomically mutate one subscriber's JSON via WATCH/MULTI. `mutate` receives
 * the parsed cJSON doc (mutable) and applies the change; helper re-serializes
 * and SETs under the transaction, retrying on conflict up to
 * OGS_REDIS_WATCH_MAX_RETRY. Returns OGS_OK/OGS_ERROR.
 */
typedef int (*redis_mutate_f)(cJSON *doc, void *data);
int redis_update_subscriber(const char *supi, redis_mutate_f mutate, void *data);

/*
 * Argument struct for the MME-update mutate function. Carries the scalar
 * fields mongoc_update_mme writes ($set mme_host/mme_realm/purge_flag, plus a
 * server-side mme_timestamp). Kept here so the unit test can build one.
 */
typedef struct redis_mme_arg_s {
    const char *host;
    const char *realm;
    bool purge;
} redis_mme_arg_t;

/*
 * Pure mutate functions for the WATCH/MULTI writers. Each edits an
 * already-parsed cJSON subscriber doc in place (no Redis), mirroring the
 * corresponding mongoc_* writer. Non-static so the unit tests can drive them
 * directly; the vtable methods pass them to redis_update_subscriber().
 *   - sqn_set:       data is uint64_t* -> set security.sqn
 *   - sqn_increment: data is NULL      -> security.sqn = (old + 32) & OGS_MAX_SQN
 *   - imeisv:        data is char*      -> set top-level scalar imeisv
 *   - mme:           data is redis_mme_arg_t* -> set mme_host/realm/timestamp/purge_flag
 */
int redis_mutate_sqn_set(cJSON *doc, void *data);
int redis_mutate_sqn_increment(cJSON *doc, void *data);
int redis_mutate_imeisv(cJSON *doc, void *data);
int redis_mutate_mme(cJSON *doc, void *data);

/* Parse functions (pure; per-file). Declared here, defined in the readers. */
int redis_parse_auth_info(const cJSON *doc, ogs_dbi_auth_info_t *out);
int redis_parse_subscription_data(const cJSON *doc, ogs_subscription_data_t *out);
int redis_parse_session_data(const cJSON *doc, const ogs_s_nssai_t *s_nssai,
        const char *dnn, ogs_session_data_t *out);
int redis_parse_msisdn_data(const cJSON *doc, ogs_msisdn_data_t *out);
int redis_parse_ims_data(const cJSON *doc, ogs_ims_data_t *out);

/* Vtable methods */
int redis_auth_info(char *supi, ogs_dbi_auth_info_t *out);
int redis_update_sqn(char *supi, uint64_t sqn);
int redis_increment_sqn(char *supi);
int redis_update_imeisv(char *supi, char *imeisv);
int redis_update_mme(char *supi, char *host, char *realm, bool purge);
int redis_subscription_data(char *supi, ogs_subscription_data_t *out);
int redis_session_data(const char *supi, const ogs_s_nssai_t *s_nssai,
        const char *dnn, ogs_session_data_t *out);
int redis_msisdn_data(char *id, ogs_msisdn_data_t *out);
int redis_ims_data(char *supi, ogs_ims_data_t *out);
int redis_watch_init(void);
int redis_poll_change_stream(void);

/*
 * Pure change-event helpers (defined in redis-watch.c, Tasks 2-3).
 * Declared here so redis-backend.c and the unit tests can reference them.
 */
uint32_t redis_event_field_from_name(const char *name);
int redis_parse_rich_event(const char *json, char **imsi_out, uint32_t *mask_out);
char *redis_keyspace_channel_to_imsi(const char *channel, const char *prefix);
void redis_watch_process_message(redisReply *reply);

#ifdef __cplusplus
}
#endif

#endif /* OGS_DBI_REDIS_INTERNAL_H */
