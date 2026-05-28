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

#include "dbctl-redis.h"

/*
 * URI parsing duplicates lib/dbi/redis/redis-backend.c:redis_parse_uri().
 * Keep the accepted syntax and defaults in sync with the backend.
 */
static int dbctl_redis_parse_uri(
        const char *uri, char **host, int *port, char **prefix)
{
    const char *p, *authority, *q;
    char *tmp, *colon, *slash, *qmark;

    ogs_assert(host);
    ogs_assert(port);
    ogs_assert(prefix);
    *host = NULL;
    *prefix = NULL;
    *port = DBCTL_REDIS_DEFAULT_PORT;

    if (!uri) {
        ogs_error("No redis URI provided");
        return OGS_ERROR;
    }

    if (!strncmp(uri, "redis://", 8)) {
        p = uri + 8;
    } else if (!strncmp(uri, "rediss://", 9)) {
        ogs_error("rediss:// (TLS) is not supported; use redis://");
        return OGS_ERROR;
    } else {
        ogs_error("Not a redis URI: %s", uri);
        return OGS_ERROR;
    }

    /* authority = host[:port], terminated by '/' or '?' or end */
    tmp = ogs_strdup(p);
    ogs_assert(tmp);
    qmark = strchr(tmp, '?');
    q = NULL;
    if (qmark) {
        *qmark = '\0';
        q = p + (qmark - tmp) + 1;
    }
    slash = strchr(tmp, '/');
    if (slash) *slash = '\0';
    authority = tmp;

    colon = strchr((char *)authority, ':');
    if (colon) {
        *colon = '\0';
        *port = atoi(colon + 1);
        if (*port <= 0) *port = DBCTL_REDIS_DEFAULT_PORT;
    }
    if (authority[0] == '\0') {
        ogs_free(tmp);
        ogs_error("redis URI missing host: %s", uri);
        return OGS_ERROR;
    }
    *host = ogs_strdup(authority);
    ogs_free(tmp);

    /* prefix from query string ?prefix=... (only recognized key) */
    *prefix = NULL;
    if (q) {
        const char *kv = strstr(q, "prefix=");
        if (kv) {
            const char *val = kv + 7;
            const char *amp = strchr(val, '&');
            size_t len = amp ? (size_t)(amp - val) : strlen(val);
            *prefix = ogs_malloc(len + 1);
            ogs_assert(*prefix);
            memcpy(*prefix, val, len);
            (*prefix)[len] = '\0';
        }
    }
    if (!*prefix) *prefix = ogs_strdup(DBCTL_REDIS_DEFAULT_PREFIX);

    return OGS_OK;
}

int dbctl_redis_open(const char *uri, dbctl_redis_t *self)
{
    ogs_assert(self);
    memset(self, 0, sizeof(*self));

    if (dbctl_redis_parse_uri(uri, &self->host, &self->port, &self->prefix)
            != OGS_OK)
        return OGS_ERROR;

    self->ctx = redisConnect(self->host, self->port);
    if (self->ctx == NULL || self->ctx->err) {
        if (self->ctx) {
            ogs_error("Failed to connect to Redis [redis://%s:%d]: %s",
                    self->host, self->port, self->ctx->errstr);
        } else {
            ogs_error("Failed to allocate redis context [redis://%s:%d]",
                    self->host, self->port);
        }
        /* Leave a clean, zeroed struct on failure (caller need not close). */
        dbctl_redis_close(self);
        return OGS_ERROR;
    }

    return OGS_OK;
}

void dbctl_redis_close(dbctl_redis_t *self)
{
    if (!self) return;

    if (self->ctx) {
        redisFree(self->ctx);
        self->ctx = NULL;
    }
    if (self->host) {
        ogs_free(self->host);
        self->host = NULL;
    }
    if (self->prefix) {
        ogs_free(self->prefix);
        self->prefix = NULL;
    }
}

char *dbctl_redis_key(const dbctl_redis_t *self,
        const char *kind, const char *id)
{
    char *key;

    ogs_assert(self);
    ogs_assert(kind);
    ogs_assert(id);

    key = ogs_msprintf("%s%s:%s", self->prefix, kind, id);
    ogs_assert(key);
    return key;
}
