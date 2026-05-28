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

#ifndef DBCTL_REDIS_H
#define DBCTL_REDIS_H

#include "ogs-core.h"
#include "cJSON.h"

#include <hiredis.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The URI parsing and key/channel conventions below mirror the Redis backend
 * in lib/dbi/redis/ (see redis-internal.h and redis_subscriber_key()): keys are
 * "<prefix>subscriber:<imsi>" and "<prefix>msisdn:<bcd>", change events publish
 * to "<prefix>events:subscriber", the default prefix is "open5gs:" and the
 * default port is 6379. These are intentionally duplicated (a shared header is
 * overkill for v1); keep them in sync with lib/dbi/redis/.
 */
#define DBCTL_REDIS_DEFAULT_PREFIX "open5gs:"
#define DBCTL_REDIS_DEFAULT_PORT 6379

typedef struct dbctl_redis_s {
    char *host;
    int port;
    char *prefix;       /* e.g. "open5gs:" */
    redisContext *ctx;  /* sync connection */
} dbctl_redis_t;

/*
 * Parse a "redis://host[:port][/db][?prefix=...]" URI and connect.
 * Defaults: port 6379, prefix "open5gs:". Non-redis schemes are rejected.
 * Returns OGS_OK on a live connection, OGS_ERROR otherwise (with a message).
 */
int dbctl_redis_open(const char *uri, dbctl_redis_t *self);

/* Close the connection and free owned strings. Safe on a zeroed struct. */
void dbctl_redis_close(dbctl_redis_t *self);

/*
 * Build a heap key string "<prefix><kind>:<id>", e.g.
 * dbctl_redis_key(self, "subscriber", imsi). Caller frees with ogs_free().
 */
char *dbctl_redis_key(const dbctl_redis_t *self,
        const char *kind, const char *id);

#ifdef __cplusplus
}
#endif

#endif /* DBCTL_REDIS_H */
