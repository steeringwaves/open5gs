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

typedef struct ogs_redis_s {
    bool initialized;
    char *host;
    int port;
    char *prefix;          /* e.g. "open5gs:" */
    char *masked_uri;
    redisContext *ctx;     /* sync connection for GET/SET/WATCH/MULTI */
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

#ifdef __cplusplus
}
#endif

#endif /* OGS_DBI_REDIS_INTERNAL_H */
