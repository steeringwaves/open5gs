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

#include <fcntl.h>
#include <errno.h>

static ogs_redis_t self;

ogs_redis_t *ogs_redis(void) { return &self; }

/*
 * Parse "redis://host[:port][/db][?prefix=...]".
 * Rejects non-redis schemes. rediss:// (TLS) is rejected for Phase 2.
 * Non-static so the unit tests can call it directly.
 */
int redis_parse_uri(const char *uri, char **host, int *port, char **prefix)
{
    const char *p, *authority, *q;
    char *tmp, *colon, *slash, *qmark;

    ogs_assert(host); ogs_assert(port); ogs_assert(prefix);
    *host = NULL; *prefix = NULL; *port = OGS_REDIS_DEFAULT_PORT;

    if (!uri) return OGS_ERROR;

    if (!strncmp(uri, "redis://", 8)) {
        p = uri + 8;
    } else if (!strncmp(uri, "rediss://", 9)) {
        ogs_error("rediss:// (TLS) is not supported yet; use redis://");
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
    if (qmark) { *qmark = '\0'; q = p + (qmark - tmp) + 1; }
    slash = strchr(tmp, '/');
    if (slash) *slash = '\0';
    authority = tmp;

    colon = strchr((char *)authority, ':');
    if (colon) {
        *colon = '\0';
        *port = atoi(colon + 1);
        if (*port <= 0) *port = OGS_REDIS_DEFAULT_PORT;
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
    if (!*prefix) *prefix = ogs_strdup(OGS_REDIS_DEFAULT_PREFIX);

    return OGS_OK;
}

int redis_init(const char *uri)
{
    memset(&self, 0, sizeof(self));

    if (redis_parse_uri(uri, &self.host, &self.port, &self.prefix) != OGS_OK)
        return OGS_ERROR;

    self.masked_uri = ogs_msprintf("redis://%s:%d", self.host, self.port);
    ogs_assert(self.masked_uri);

    self.ctx = redisConnect(self.host, self.port);
    if (self.ctx == NULL || self.ctx->err) {
        if (self.ctx) {
            ogs_warn("Failed to connect to Redis [%s]: %s",
                    self.masked_uri, self.ctx->errstr);
            redisFree(self.ctx);
            self.ctx = NULL;
        } else {
            ogs_warn("Failed to allocate redis context [%s]", self.masked_uri);
        }
        return OGS_RETRY;   /* mirror mongoc: NF retries init */
    }

    self.initialized = true;
    ogs_info("Redis URI: '%s' (prefix '%s')", self.masked_uri, self.prefix);
    return OGS_OK;
}

void redis_final(void)
{
    if (self.sub_ctx) { redisFree(self.sub_ctx); self.sub_ctx = NULL; }
    if (self.ctx) { redisFree(self.ctx); self.ctx = NULL; }
    if (self.host) { ogs_free(self.host); self.host = NULL; }
    if (self.prefix) { ogs_free(self.prefix); self.prefix = NULL; }
    if (self.masked_uri) { ogs_free(self.masked_uri); self.masked_uri = NULL; }
    self.initialized = false;
}

char *redis_subscriber_key(const char *supi)
{
    char *supi_type = NULL, *supi_id = NULL, *key = NULL;

    ogs_assert(supi);
    if (ogs_id_get_type_value((char *)supi, &supi_type, &supi_id) == false) {
        ogs_error("Invalid supi=%s", supi);
        return NULL;
    }
    key = ogs_msprintf("%ssubscriber:%s", ogs_redis()->prefix, supi_id);
    ogs_assert(key);
    ogs_free(supi_type);
    ogs_free(supi_id);
    return key;
}

char *redis_get_string(const char *key)
{
    redisReply *reply;
    char *out = NULL;

    ogs_assert(ogs_redis()->ctx);
    reply = redisCommand(ogs_redis()->ctx, "GET %s", key);
    if (!reply) {
        ogs_error("Redis GET failed (no reply) for key %s", key);
        return NULL;
    }
    if (reply->type == REDIS_REPLY_STRING && reply->len > 0)
        out = ogs_strdup(reply->str);
    freeReplyObject(reply);
    return out;
}

int redis_update_subscriber(const char *supi, redis_mutate_f mutate, void *data)
{
    char *key;
    int attempt, rv = OGS_ERROR;

    key = redis_subscriber_key(supi);
    if (!key) return OGS_ERROR;

    for (attempt = 0; attempt < OGS_REDIS_WATCH_MAX_RETRY; attempt++) {
        redisReply *reply;
        char *json;
        cJSON *doc;
        char *serialized;

        /* WATCH key */
        reply = redisCommand(ogs_redis()->ctx, "WATCH %s", key);
        if (!reply) goto done;
        freeReplyObject(reply);

        json = redis_get_string(key);
        if (!json) {
            ogs_error("[%s] not found for update", supi);
            reply = redisCommand(ogs_redis()->ctx, "UNWATCH");
            if (reply) freeReplyObject(reply);
            goto done;
        }
        doc = cJSON_Parse(json);
        ogs_free(json);
        if (!doc) {
            ogs_error("[%s] malformed JSON for update", supi);
            reply = redisCommand(ogs_redis()->ctx, "UNWATCH");
            if (reply) freeReplyObject(reply);
            goto done;
        }

        if (mutate(doc, data) != OGS_OK) {
            cJSON_Delete(doc);
            reply = redisCommand(ogs_redis()->ctx, "UNWATCH");
            if (reply) freeReplyObject(reply);
            goto done;
        }

        serialized = cJSON_PrintUnformatted(doc);
        cJSON_Delete(doc);
        ogs_assert(serialized);

        /* MULTI; SET key serialized; EXEC */
        reply = redisCommand(ogs_redis()->ctx, "MULTI");
        if (!reply || reply->type == REDIS_REPLY_ERROR) {
            if (reply) {
                ogs_error("[%s] redis MULTI failed: %s", supi, reply->str);
                freeReplyObject(reply);
            }
            goto done;
        }
        freeReplyObject(reply);
        reply = redisCommand(ogs_redis()->ctx, "SET %s %s", key, serialized);
        if (reply) freeReplyObject(reply);
        cJSON_free(serialized);   /* matches the allocator cJSON was built with */

        reply = redisCommand(ogs_redis()->ctx, "EXEC");
        if (!reply) goto done;
        /* EXEC returns nil (array) on WATCH conflict -> retry */
        if (reply->type == REDIS_REPLY_NIL ||
                (reply->type == REDIS_REPLY_ARRAY && reply->elements == 0)) {
            freeReplyObject(reply);
            ogs_warn("[%s] redis update conflict, retry %d", supi, attempt + 1);
            continue;
        }
        if (reply->type == REDIS_REPLY_ERROR) {
            ogs_error("[%s] redis EXEC failed: %s", supi, reply->str);
            freeReplyObject(reply);
            goto done;
        }
        freeReplyObject(reply);
        rv = OGS_OK;
        goto done;
    }
    ogs_error("[%s] redis update failed after %d retries",
            supi, OGS_REDIS_WATCH_MAX_RETRY);
done:
    ogs_free(key);
    return rv;
}

const ogs_dbi_backend_t redis_backend = {
    .name = "redis", .scheme = "redis",
    .init = redis_init, .final = redis_final,
    .auth_info = redis_auth_info,
    .update_sqn = redis_update_sqn,
    .increment_sqn = redis_increment_sqn,
    .update_imeisv = redis_update_imeisv,
    .update_mme = redis_update_mme,
    .subscription_data = redis_subscription_data,
    .session_data = redis_session_data,
    .msisdn_data = redis_msisdn_data,
    .ims_data = redis_ims_data,
    .watch_init = redis_watch_init,
    .poll_change_stream = redis_poll_change_stream,
};

int redis_watch_init(void)
{
    redisReply *reply;
    int flags;

    if (!ogs_redis()->ctx) {
        ogs_error("redis_watch_init: backend not connected");
        return OGS_ERROR;
    }

    ogs_redis()->sub_ctx = redisConnect(ogs_redis()->host, ogs_redis()->port);
    if (!ogs_redis()->sub_ctx || ogs_redis()->sub_ctx->err) {
        ogs_warn("redis_watch_init: failed to open subscriber connection: %s",
                ogs_redis()->sub_ctx ? ogs_redis()->sub_ctx->errstr : "alloc");
        if (ogs_redis()->sub_ctx) {
            redisFree(ogs_redis()->sub_ctx);
            ogs_redis()->sub_ctx = NULL;
        }
        return OGS_ERROR;
    }

    /* Best-effort enable keyspace notifications (generic + string + key-event).
     * On managed Redis where CONFIG is disabled this fails — warn, continue. */
    reply = redisCommand(ogs_redis()->sub_ctx,
            "CONFIG SET notify-keyspace-events Kg$");
    if (!reply || reply->type == REDIS_REPLY_ERROR) {
        ogs_warn("redis: could not enable keyspace notifications "
                 "(set 'notify-keyspace-events Kg$' in redis.conf); "
                 "subscriber edits may not propagate to HSS");
    }
    if (reply) freeReplyObject(reply);

    /* Rich channel (precise field updates, published by open5gs-dbctl). */
    reply = redisCommand(ogs_redis()->sub_ctx, "SUBSCRIBE %s%s",
            ogs_redis()->prefix, OGS_REDIS_EVENTS_CHANNEL_SUFFIX);
    if (reply) freeReplyObject(reply);

    /* Keyspace fallback for third-party writers. */
    reply = redisCommand(ogs_redis()->sub_ctx,
            "PSUBSCRIBE __keyspace@*__:%ssubscriber:*", ogs_redis()->prefix);
    if (reply) freeReplyObject(reply);

    /* Non-blocking so the HSS poll-timer drain never blocks. */
    flags = fcntl(ogs_redis()->sub_ctx->fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(ogs_redis()->sub_ctx->fd, F_SETFL, flags | O_NONBLOCK);

    ogs_redis()->suppress_next = 0;
    memset(ogs_redis()->suppress, 0, sizeof(ogs_redis()->suppress));

    ogs_info("Redis change notifications enabled (rich + keyspace).");
    return OGS_OK;
}

int redis_poll_change_stream(void)
{
    void *vreply;

    if (!ogs_redis()->sub_ctx)
        return OGS_ERROR;

    if (redisBufferRead(ogs_redis()->sub_ctx) == REDIS_ERR) {
        if (ogs_redis()->sub_ctx->err == REDIS_ERR_IO &&
                (errno == EAGAIN || errno == EWOULDBLOCK)) {
            ogs_redis()->sub_ctx->err = 0;
            ogs_redis()->sub_ctx->errstr[0] = '\0';
        } else {
            ogs_warn("redis watch read error: %s", ogs_redis()->sub_ctx->errstr);
            return OGS_ERROR;
        }
    }

    while (redisGetReplyFromReader(ogs_redis()->sub_ctx, &vreply) == REDIS_OK
            && vreply) {
        redis_watch_process_message((redisReply *)vreply);
        freeReplyObject((redisReply *)vreply);
    }
    return OGS_OK;
}
