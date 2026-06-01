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
#include "dbctl-subscriber.h"

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

int dbctl_redis_set_subscriber(dbctl_redis_t *self,
        const char *imsi, const cJSON *doc)
{
    char *key, *json;
    redisReply *reply;
    int rv = OGS_ERROR;

    ogs_assert(self);
    ogs_assert(imsi);
    ogs_assert(doc);

    json = cJSON_PrintUnformatted((cJSON *)doc);
    if (!json) {
        ogs_error("Failed to serialize subscriber [%s]", imsi);
        return OGS_ERROR;
    }

    key = dbctl_redis_key(self, "subscriber", imsi);

    reply = redisCommand(self->ctx, "SET %s %s", key, json);
    if (reply == NULL) {
        ogs_error("SET %s failed: %s", key, self->ctx->errstr);
    } else if (reply->type == REDIS_REPLY_ERROR) {
        ogs_error("SET %s error: %s", key, reply->str);
    } else {
        rv = OGS_OK;
    }

    if (reply) freeReplyObject(reply);
    ogs_free(key);
    cJSON_free(json);
    return rv;
}

char *dbctl_redis_get(dbctl_redis_t *self, const char *kind, const char *id)
{
    char *key, *value = NULL;
    redisReply *reply;

    ogs_assert(self);
    ogs_assert(kind);
    ogs_assert(id);

    key = dbctl_redis_key(self, kind, id);

    reply = redisCommand(self->ctx, "GET %s", key);
    if (reply == NULL) {
        ogs_error("GET %s failed: %s", key, self->ctx->errstr);
    } else if (reply->type == REDIS_REPLY_STRING) {
        value = ogs_strdup(reply->str);
        ogs_assert(value);
    } else if (reply->type == REDIS_REPLY_ERROR) {
        ogs_error("GET %s error: %s", key, reply->str);
    }
    /* REDIS_REPLY_NIL (key absent) -> value stays NULL, no error logged */

    if (reply) freeReplyObject(reply);
    ogs_free(key);
    return value;
}

int dbctl_redis_set_msisdn_index(dbctl_redis_t *self,
        const char *msisdn, const char *imsi)
{
    char *key;
    redisReply *reply;
    int rv = OGS_ERROR;

    ogs_assert(self);
    ogs_assert(msisdn);
    ogs_assert(imsi);

    key = dbctl_redis_key(self, "msisdn", msisdn);

    reply = redisCommand(self->ctx, "SET %s %s", key, imsi);
    if (reply == NULL) {
        ogs_error("SET %s failed: %s", key, self->ctx->errstr);
    } else if (reply->type == REDIS_REPLY_ERROR) {
        ogs_error("SET %s error: %s", key, reply->str);
    } else {
        rv = OGS_OK;
    }

    if (reply) freeReplyObject(reply);
    ogs_free(key);
    return rv;
}

int dbctl_redis_del(dbctl_redis_t *self, const char *kind, const char *id)
{
    char *key;
    redisReply *reply;
    int rv = OGS_ERROR;

    ogs_assert(self);
    ogs_assert(kind);
    ogs_assert(id);

    key = dbctl_redis_key(self, kind, id);

    reply = redisCommand(self->ctx, "DEL %s", key);
    if (reply == NULL) {
        ogs_error("DEL %s failed: %s", key, self->ctx->errstr);
    } else if (reply->type == REDIS_REPLY_ERROR) {
        ogs_error("DEL %s error: %s", key, reply->str);
    } else {
        /* INTEGER reply = number of keys removed (0 if absent); both OK. */
        rv = OGS_OK;
    }

    if (reply) freeReplyObject(reply);
    ogs_free(key);
    return rv;
}

int dbctl_redis_publish_change(dbctl_redis_t *self,
        const char *imsi, const char *const *fields, int nfields)
{
    char *channel, *payload;
    redisReply *reply;
    int rv = OGS_ERROR;

    ogs_assert(self);
    ogs_assert(imsi);

    payload = dbctl_build_change_payload(imsi, fields, nfields);
    if (!payload) {
        ogs_error("Failed to build change-event payload for [%s]", imsi);
        return OGS_ERROR;
    }

    /* Channel mirrors lib/dbi/redis/ : "<prefix>events:subscriber". */
    channel = ogs_msprintf("%sevents:subscriber", self->prefix);
    ogs_assert(channel);

    reply = redisCommand(self->ctx, "PUBLISH %s %s", channel, payload);
    if (reply == NULL) {
        ogs_error("PUBLISH %s failed: %s", channel, self->ctx->errstr);
    } else if (reply->type == REDIS_REPLY_ERROR) {
        ogs_error("PUBLISH %s error: %s", channel, reply->str);
    } else {
        rv = OGS_OK;
    }

    if (reply) freeReplyObject(reply);
    ogs_free(channel);
    cJSON_free(payload);
    return rv;
}

int dbctl_redis_scan_imsis(dbctl_redis_t *self,
        void (*cb)(const char *imsi, void *data), void *data, int limit)
{
    char *match;
    size_t prefix_len;
    unsigned long long cursor = 0;
    int count = 0;

    ogs_assert(self);
    ogs_assert(cb);

    match = ogs_msprintf("%ssubscriber:*", self->prefix);
    ogs_assert(match);
    /* the imsi is everything after "<prefix>subscriber:" */
    prefix_len = strlen(self->prefix) + strlen("subscriber:");

    do {
        redisReply *reply, *keys;
        size_t i;

        reply = redisCommand(self->ctx,
                "SCAN %llu MATCH %s COUNT 100", cursor, match);
        if (reply == NULL) {
            ogs_error("SCAN failed: %s", self->ctx->errstr);
            ogs_free(match);
            return OGS_ERROR;
        }
        if (reply->type == REDIS_REPLY_ERROR) {
            ogs_error("SCAN error: %s", reply->str);
            freeReplyObject(reply);
            ogs_free(match);
            return OGS_ERROR;
        }
        if (reply->type != REDIS_REPLY_ARRAY || reply->elements != 2) {
            ogs_error("SCAN returned an unexpected reply");
            freeReplyObject(reply);
            ogs_free(match);
            return OGS_ERROR;
        }

        cursor = strtoull(reply->element[0]->str, NULL, 10);
        keys = reply->element[1];

        for (i = 0; i < keys->elements; i++) {
            const char *key = keys->element[i]->str;
            if (!key)
                continue;
            if (strlen(key) <= prefix_len)
                continue;   /* not actually "<prefix>subscriber:<imsi>" */
            cb(key + prefix_len, data);
            count++;
            if (limit > 0 && count >= limit) {
                freeReplyObject(reply);
                ogs_free(match);
                return count;
            }
        }

        freeReplyObject(reply);
    } while (cursor != 0);

    ogs_free(match);
    return count;
}
