/*
 * Diagnostic state — see diagnostic-state.h for the design overview.
 *
 * Implementation notes:
 *  - Lazy connection: nothing happens until the first set/del call. The
 *    daemon doesn't pay for the feature when DIAG_REDIS_URL is unset.
 *  - Single connection held open between calls; reconnect on failure.
 *  - Single mutex serialises all Redis I/O — same pattern as
 *    lib/dbi/ogs-flatfile-state.c. Diagnostic events are infrequent
 *    enough that a global lock is not a bottleneck.
 *  - All commands use redisCommand(...) which is synchronous; failures
 *    are logged via stderr (libcore can't depend on the ogs_log
 *    domains) and the daemon keeps running.
 */

#include "ogs-core.h"
#include "diagnostic-state.h"

#include <hiredis/hiredis.h>

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int __ogs_diag_domain;

#undef OGS_LOG_DOMAIN
#define OGS_LOG_DOMAIN __ogs_diag_domain

#define DIAG_STATE_PREFIX "open5gs:live"

typedef struct diag_state_s {
    bool initialized;
    bool enabled;             /* set by diagnostic_state_configure() */
    bool connected;           /* true between a successful connect and the
                               * next close — gates the "connected" info
                               * log so it only fires once per cycle */
    redisContext *redis;
    char *uri;                /* full URL kept for re-parse on configure() */
    char *host;
    int port;
    int db;
    char *password;
    pthread_mutex_t lock;
} diag_state_t;

static diag_state_t self;

/* ---- URL parsing (mirrors ogs-flatfile-state.c parse_redis_uri) ---- */
static int parse_redis_uri(const char *uri,
        char **host, int *port, int *db, char **password)
{
    const char *p;
    char *tmp, *at, *slash, *colon;

    if (!uri || strncmp(uri, "redis://", 8) != 0) return -1;
    p = uri + 8;

    *host = NULL;
    *port = 6379;
    *db = 0;
    *password = NULL;

    tmp = strdup(p);
    if (!tmp) return -1;

    slash = strchr(tmp, '/');
    if (slash) {
        *slash = '\0';
        if (*(slash + 1)) *db = atoi(slash + 1);
    }

    at = strrchr(tmp, '@');
    if (at) {
        *at = '\0';
        colon = strchr(tmp, ':');
        *password = strdup(colon ? colon + 1 : tmp);
        p = at + 1;
    } else {
        p = tmp;
    }

    colon = strrchr((char *)p, ':');
    if (colon) { *colon = '\0'; *port = atoi(colon + 1); }
    *host = strdup(p);

    free(tmp);
    return (*host && (*host)[0]) ? 0 : -1;
}

/* ---- Connection management (lazy, reconnect on failure) ---- */
static void close_redis_locked(void)
{
    if (self.redis) {
        redisFree(self.redis);
        self.redis = NULL;
    }
    self.connected = false;
}

static int ensure_redis_locked(void)
{
    redisReply *r;

    if (!self.enabled || !self.host) return -1;

    if (self.redis && self.redis->err == 0) return 0;
    close_redis_locked();

    self.redis = redisConnect(self.host, self.port);
    if (!self.redis || self.redis->err) {
        ogs_error("state: connect to %s:%d failed: %s",
                self.host, self.port,
                self.redis ? self.redis->errstr : "(alloc failed)");
        if (self.redis) {
            redisFree(self.redis);
            self.redis = NULL;
        }
        return -1;
    }

    if (self.password) {
        r = redisCommand(self.redis, "AUTH %s", self.password);
        if (!r || r->type == REDIS_REPLY_ERROR) {
            ogs_error("state: AUTH against %s:%d failed: %s",
                    self.host, self.port,
                    (r && r->str) ? r->str : "(no reply)");
            if (r) freeReplyObject(r);
            close_redis_locked();
            return -1;
        }
        freeReplyObject(r);
    }

    if (self.db) {
        r = redisCommand(self.redis, "SELECT %d", self.db);
        if (!r || r->type == REDIS_REPLY_ERROR) {
            ogs_error("state: SELECT %d on %s:%d failed: %s",
                    self.db, self.host, self.port,
                    (r && r->str) ? r->str : "(no reply)");
            if (r) freeReplyObject(r);
            close_redis_locked();
            return -1;
        }
        freeReplyObject(r);
    }

    /* One-shot info log — fires on the first successful connect of each
     * configure cycle. Reconnects after a Redis bounce will log again,
     * so log lines reflect actual state transitions. */
    if (!self.connected) {
        ogs_info("state: connected to redis %s:%d/%d",
                self.host, self.port, self.db);
        self.connected = true;
    }
    return 0;
}

/* ---- Public init / configure / final ---- */
void diagnostic_state_init(void)
{
    if (self.initialized) return;
    pthread_mutex_init(&self.lock, NULL);
    self.initialized = true;
    /* enabled stays false until diagnostic_state_configure() is called. */
}

void diagnostic_state_configure(bool enabled, const char *redis_url)
{
    if (!self.initialized) diagnostic_state_init();

    pthread_mutex_lock(&self.lock);

    /* Drop any prior connection + parsed URI before honouring the new
     * configuration — caller may have flipped the redis URL. */
    close_redis_locked();
    if (self.uri)      { free(self.uri);      self.uri = NULL; }
    if (self.host)     { free(self.host);     self.host = NULL; }
    if (self.password) { free(self.password); self.password = NULL; }
    self.port = 0;
    self.db = 0;

    self.enabled = enabled && redis_url && *redis_url;
    if (!self.enabled) {
        ogs_info("state: disabled (state.enabled=false or no redis URL)");
        pthread_mutex_unlock(&self.lock);
        return;
    }

    self.uri = strdup(redis_url);
    if (parse_redis_uri(redis_url, &self.host, &self.port,
                &self.db, &self.password) != 0) {
        ogs_error("state: invalid redis URL '%s' — feature disabled",
                redis_url);
        self.enabled = false;
        if (self.uri) { free(self.uri); self.uri = NULL; }
        pthread_mutex_unlock(&self.lock);
        return;
    }

    /* Boot-time confirmation that the YAML knobs landed. The actual
     * TCP connect is deferred until the first set/del call —
     * "connected to redis ..." follows from ensure_redis_locked(). */
    ogs_info("state: enabled, redis target %s:%d/%d (lazy connect)",
            self.host, self.port, self.db);

    pthread_mutex_unlock(&self.lock);
}

void diagnostic_state_final(void)
{
    if (!self.initialized) return;
    pthread_mutex_lock(&self.lock);
    close_redis_locked();
    if (self.uri)      { free(self.uri);      self.uri = NULL; }
    if (self.host)     { free(self.host);     self.host = NULL; }
    if (self.password) { free(self.password); self.password = NULL; }
    self.enabled = false;
    self.initialized = false;
    pthread_mutex_unlock(&self.lock);
    pthread_mutex_destroy(&self.lock);
}

/* ---- Issue a command, swallow the reply, log on error ---- */
static void exec_locked(const char *fmt, ...)
{
    va_list ap;
    redisReply *r;

    if (ensure_redis_locked() != 0) return;

    va_start(ap, fmt);
    r = redisvCommand(self.redis, fmt, ap);
    va_end(ap);

    if (!r) {
        /* Hiredis sets self.redis->err on transport failure. Drop the
         * context so the next call reconnects. The connect-failure log
         * fires from ensure_redis_locked() on the next attempt. */
        ogs_warn("state: redis command failed: %s",
                self.redis ? self.redis->errstr : "(unknown)");
        close_redis_locked();
        return;
    }
    if (r->type == REDIS_REPLY_ERROR)
        ogs_warn("state: redis replied with error: %s", r->str);
    freeReplyObject(r);
}

static const char *or_empty(const char *s) { return s ? s : ""; }

/* ---- gNB / eNB ---- */
void diagnostic_state_gnb_set(const char *address)
{
    if (!address) return;
    pthread_mutex_lock(&self.lock);
    exec_locked("HSET " DIAG_STATE_PREFIX ":gnb:%s "
            "address %s connected_at %lld",
            address, address, (long long)time(NULL));
    pthread_mutex_unlock(&self.lock);
}

void diagnostic_state_gnb_del(const char *address)
{
    if (!address) return;
    pthread_mutex_lock(&self.lock);
    exec_locked("DEL " DIAG_STATE_PREFIX ":gnb:%s", address);
    pthread_mutex_unlock(&self.lock);
}

void diagnostic_state_enb_set(const char *address)
{
    if (!address) return;
    pthread_mutex_lock(&self.lock);
    exec_locked("HSET " DIAG_STATE_PREFIX ":enb:%s "
            "address %s connected_at %lld",
            address, address, (long long)time(NULL));
    pthread_mutex_unlock(&self.lock);
}

void diagnostic_state_enb_del(const char *address)
{
    if (!address) return;
    pthread_mutex_lock(&self.lock);
    exec_locked("DEL " DIAG_STATE_PREFIX ":enb:%s", address);
    pthread_mutex_unlock(&self.lock);
}

/* ---- UE attach state ---- */
void diagnostic_state_ue_set(const char *imsi, const char *imei,
        const char *supi, const char *suci)
{
    if (!imsi || !*imsi) return;
    pthread_mutex_lock(&self.lock);
    exec_locked("HSET " DIAG_STATE_PREFIX ":ue:%s "
            "imsi %s imei %s supi %s suci %s attached_at %lld",
            imsi, imsi, or_empty(imei), or_empty(supi),
            or_empty(suci), (long long)time(NULL));
    pthread_mutex_unlock(&self.lock);
}

void diagnostic_state_ue_del(const char *imsi)
{
    if (!imsi || !*imsi) return;
    pthread_mutex_lock(&self.lock);
    exec_locked("DEL " DIAG_STATE_PREFIX ":ue:%s", imsi);
    pthread_mutex_unlock(&self.lock);
}

/* ---- Per-session state ---- */
void diagnostic_state_session_set(const char *imsi, const char *apn,
        const char *imei, const char *supi,
        const char *ipv4, const char *ipv6)
{
    if (!imsi || !*imsi || !apn || !*apn) return;
    pthread_mutex_lock(&self.lock);
    exec_locked("HSET " DIAG_STATE_PREFIX ":session:%s:%s "
            "imsi %s apn %s imei %s supi %s ipv4 %s ipv6 %s created_at %lld",
            imsi, apn,
            imsi, apn, or_empty(imei), or_empty(supi),
            or_empty(ipv4), or_empty(ipv6), (long long)time(NULL));
    pthread_mutex_unlock(&self.lock);
}

void diagnostic_state_session_del(const char *imsi, const char *apn)
{
    if (!imsi || !*imsi || !apn || !*apn) return;
    pthread_mutex_lock(&self.lock);
    exec_locked("DEL " DIAG_STATE_PREFIX ":session:%s:%s", imsi, apn);
    pthread_mutex_unlock(&self.lock);
}
