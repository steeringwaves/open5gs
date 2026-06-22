/*
 * Mongoless: per-subscriber mutable state store.
 *
 * Two backends:
 *   - Redis (via hiredis): one HASH per subscriber keyed by IMSI,
 *     with fields `sqn`, `mme_host`, `mme_realm`, `purge_flag`, `imeisv`.
 *   - In-memory: ogs_hash_t<imsi -> state_entry_t*>.
 *
 * Selection: ogs_flatfile_state_init("redis://...") connects to Redis;
 * any other value (or NULL) keeps state in memory only.
 *
 * Isolated from upstream files.
 */

#include "ogs-dbi.h"

#include <hiredis/hiredis.h>

typedef struct state_entry_s {
    uint64_t sqn;
    bool has_sqn;
    char *mme_host;
    char *mme_realm;
    bool purge_flag;
    bool has_mme;
    char *imeisv;
} state_entry_t;

static struct {
    bool initialized;
    redisContext *redis;
    char *redis_host;
    int redis_port;
    int redis_db;
    char *redis_password;
    ogs_thread_mutex_t lock;
    ogs_hash_t *memory; /* imsi -> state_entry_t* */
} self;

static void state_entry_free(state_entry_t *e)
{
    if (!e) return;
    if (e->mme_host) ogs_free(e->mme_host);
    if (e->mme_realm) ogs_free(e->mme_realm);
    if (e->imeisv) ogs_free(e->imeisv);
    ogs_free(e);
}

static state_entry_t *memory_get_or_create(const char *imsi, bool create)
{
    state_entry_t *e = ogs_hash_get(self.memory, imsi, strlen(imsi));
    if (e || !create) return e;
    e = ogs_calloc(1, sizeof(*e));
    ogs_assert(e);
    char *key = ogs_strdup(imsi);
    ogs_assert(key);
    ogs_hash_set(self.memory, key, strlen(key), e);
    return e;
}

/*
 * URI parsing for redis://[:password@]host[:port][/db]
 * Returns OGS_OK on success.
 */
static int parse_redis_uri(const char *uri,
        char **host, int *port, int *db, char **password)
{
    const char *p;
    char *tmp, *at, *slash, *colon;

    if (!uri) return OGS_ERROR;
    if (strncmp(uri, "redis://", 8) != 0) {
        ogs_error("State URI is not redis://: %s", uri);
        return OGS_ERROR;
    }
    p = uri + 8;

    *host = NULL;
    *port = 6379;
    *db = 0;
    *password = NULL;

    tmp = ogs_strdup(p);
    ogs_assert(tmp);

    /* db suffix: /<n> */
    slash = strchr(tmp, '/');
    if (slash) {
        *slash = '\0';
        if (*(slash + 1))
            *db = atoi(slash + 1);
    }

    /* user/password prefix: [user]:password@ */
    at = strrchr(tmp, '@');
    if (at) {
        *at = '\0';
        colon = strchr(tmp, ':');
        if (colon)
            *password = ogs_strdup(colon + 1);
        else
            *password = ogs_strdup(tmp);
        ogs_assert(*password);
        p = at + 1;
    } else {
        p = tmp;
    }

    /* host[:port] */
    colon = strrchr((char *)p, ':');
    if (colon) {
        *colon = '\0';
        *port = atoi(colon + 1);
    }
    *host = ogs_strdup(p);
    ogs_assert(*host);

    ogs_free(tmp);
    return OGS_OK;
}

static int redis_connect(void)
{
    redisContext *ctx;

    ctx = redisConnect(self.redis_host, self.redis_port);
    if (!ctx || ctx->err) {
        ogs_error("Redis connect failed [%s:%d]: %s",
                self.redis_host, self.redis_port,
                ctx ? ctx->errstr : "alloc failed");
        if (ctx) redisFree(ctx);
        return OGS_ERROR;
    }

    if (self.redis_password) {
        redisReply *r = redisCommand(ctx, "AUTH %s", self.redis_password);
        if (!r || r->type == REDIS_REPLY_ERROR) {
            ogs_error("Redis AUTH failed: %s",
                    r ? r->str : "no reply");
            if (r) freeReplyObject(r);
            redisFree(ctx);
            return OGS_ERROR;
        }
        freeReplyObject(r);
    }

    if (self.redis_db) {
        redisReply *r = redisCommand(ctx, "SELECT %d", self.redis_db);
        if (!r || r->type == REDIS_REPLY_ERROR) {
            ogs_error("Redis SELECT %d failed: %s",
                    self.redis_db, r ? r->str : "no reply");
            if (r) freeReplyObject(r);
            redisFree(ctx);
            return OGS_ERROR;
        }
        freeReplyObject(r);
    }

    self.redis = ctx;
    return OGS_OK;
}

/* Reconnect on transient failure. Returns OGS_OK if context is usable. */
static int redis_ensure(void)
{
    if (self.redis && self.redis->err == 0) return OGS_OK;
    if (self.redis) {
        redisFree(self.redis);
        self.redis = NULL;
    }
    return redis_connect();
}

static const char *state_key(const char *imsi, char *buf, size_t buflen)
{
    snprintf(buf, buflen, "open5gs:sub:%s", imsi);
    return buf;
}

int ogs_flatfile_state_init(const char *uri)
{
    memset(&self, 0, sizeof(self));
    ogs_thread_mutex_init(&self.lock);
    self.memory = ogs_hash_make();
    ogs_assert(self.memory);

    if (!uri || !*uri) {
        ogs_info("State backend: in-memory (no persistence)");
        self.initialized = true;
        return OGS_OK;
    }

    if (parse_redis_uri(uri, &self.redis_host, &self.redis_port,
                &self.redis_db, &self.redis_password) != OGS_OK) {
        return OGS_ERROR;
    }

    if (redis_connect() != OGS_OK) {
        ogs_error("State backend Redis unavailable at %s:%d — "
                "state will be in-memory only",
                self.redis_host, self.redis_port);
        /* Continue without redis, fall through to in-memory only. */
    } else {
        ogs_info("State backend: Redis %s:%d/%d",
                self.redis_host, self.redis_port, self.redis_db);
    }

    self.initialized = true;
    return OGS_OK;
}

void ogs_flatfile_state_final(void)
{
    if (!self.initialized) return;

    if (self.redis) {
        redisFree(self.redis);
        self.redis = NULL;
    }

    if (self.memory) {
        ogs_hash_index_t *hi;
        for (hi = ogs_hash_first(self.memory); hi; hi = ogs_hash_next(hi)) {
            const void *key;
            int klen;
            void *val;
            ogs_hash_this(hi, &key, &klen, &val);
            state_entry_free((state_entry_t *)val);
            ogs_hash_set(self.memory, key, klen, NULL);
            ogs_free((void *)key);
        }
        ogs_hash_destroy(self.memory);
        self.memory = NULL;
    }

    if (self.redis_host) ogs_free(self.redis_host);
    if (self.redis_password) ogs_free(self.redis_password);

    ogs_thread_mutex_destroy(&self.lock);
    memset(&self, 0, sizeof(self));
}

/* -------- SQN -------- */

int ogs_flatfile_state_get_sqn(const char *imsi, uint64_t *out_sqn)
{
    int rv = OGS_NOTFOUND;
    char key[64];

    ogs_assert(imsi);
    ogs_assert(out_sqn);

    ogs_thread_mutex_lock(&self.lock);

    if (self.redis && redis_ensure() == OGS_OK) {
        redisReply *r = redisCommand(self.redis,
                "HGET %s sqn", state_key(imsi, key, sizeof(key)));
        if (r && r->type == REDIS_REPLY_STRING) {
            *out_sqn = strtoull(r->str, NULL, 10);
            rv = OGS_OK;
        }
        if (r) freeReplyObject(r);
        if (rv == OGS_OK) goto out;
    }

    state_entry_t *e = memory_get_or_create(imsi, false);
    if (e && e->has_sqn) {
        *out_sqn = e->sqn;
        rv = OGS_OK;
    }
out:
    ogs_thread_mutex_unlock(&self.lock);
    return rv;
}

int ogs_flatfile_state_set_sqn(const char *imsi, uint64_t sqn)
{
    char key[64];

    ogs_assert(imsi);

    ogs_thread_mutex_lock(&self.lock);

    if (self.redis && redis_ensure() == OGS_OK) {
        redisReply *r = redisCommand(self.redis,
                "HSET %s sqn %llu",
                state_key(imsi, key, sizeof(key)),
                (unsigned long long)sqn);
        if (r) freeReplyObject(r);
    }

    state_entry_t *e = memory_get_or_create(imsi, true);
    e->sqn = sqn;
    e->has_sqn = true;

    ogs_thread_mutex_unlock(&self.lock);
    return OGS_OK;
}

int ogs_flatfile_state_increment_sqn(const char *imsi)
{
    uint64_t cur = 0;
    int have = ogs_flatfile_state_get_sqn(imsi, &cur);
    if (have != OGS_OK) cur = 0;
    cur = (cur + 32) & OGS_MAX_SQN;
    return ogs_flatfile_state_set_sqn(imsi, cur);
}

/* -------- MME host/realm + purge flag -------- */

int ogs_flatfile_state_get_mme(const char *imsi,
        char **out_host, char **out_realm, bool *out_purge_flag)
{
    int rv = OGS_NOTFOUND;
    char key[64];

    ogs_assert(imsi);

    ogs_thread_mutex_lock(&self.lock);

    if (self.redis && redis_ensure() == OGS_OK) {
        redisReply *r = redisCommand(self.redis,
                "HMGET %s mme_host mme_realm purge_flag",
                state_key(imsi, key, sizeof(key)));
        if (r && r->type == REDIS_REPLY_ARRAY && r->elements == 3) {
            if (out_host && r->element[0]->type == REDIS_REPLY_STRING)
                *out_host = ogs_strdup(r->element[0]->str);
            if (out_realm && r->element[1]->type == REDIS_REPLY_STRING)
                *out_realm = ogs_strdup(r->element[1]->str);
            if (out_purge_flag && r->element[2]->type == REDIS_REPLY_STRING)
                *out_purge_flag = (r->element[2]->str[0] == '1');
            if (r->element[0]->type == REDIS_REPLY_STRING ||
                r->element[1]->type == REDIS_REPLY_STRING ||
                r->element[2]->type == REDIS_REPLY_STRING)
                rv = OGS_OK;
        }
        if (r) freeReplyObject(r);
        if (rv == OGS_OK) goto out;
    }

    state_entry_t *e = memory_get_or_create(imsi, false);
    if (e && e->has_mme) {
        if (out_host && e->mme_host) *out_host = ogs_strdup(e->mme_host);
        if (out_realm && e->mme_realm) *out_realm = ogs_strdup(e->mme_realm);
        if (out_purge_flag) *out_purge_flag = e->purge_flag;
        rv = OGS_OK;
    }
out:
    ogs_thread_mutex_unlock(&self.lock);
    return rv;
}

int ogs_flatfile_state_set_mme(const char *imsi,
        const char *host, const char *realm, bool purge_flag)
{
    char key[64];

    ogs_assert(imsi);

    ogs_thread_mutex_lock(&self.lock);

    if (self.redis && redis_ensure() == OGS_OK) {
        redisReply *r = redisCommand(self.redis,
                "HSET %s mme_host %s mme_realm %s purge_flag %d",
                state_key(imsi, key, sizeof(key)),
                host ? host : "",
                realm ? realm : "",
                purge_flag ? 1 : 0);
        if (r) freeReplyObject(r);
    }

    state_entry_t *e = memory_get_or_create(imsi, true);
    if (e->mme_host) ogs_free(e->mme_host);
    e->mme_host = host ? ogs_strdup(host) : NULL;
    if (e->mme_realm) ogs_free(e->mme_realm);
    e->mme_realm = realm ? ogs_strdup(realm) : NULL;
    e->purge_flag = purge_flag;
    e->has_mme = true;

    ogs_thread_mutex_unlock(&self.lock);
    return OGS_OK;
}

/* -------- IMEISV -------- */

int ogs_flatfile_state_get_imeisv(const char *imsi, char **out_imeisv)
{
    int rv = OGS_NOTFOUND;
    char key[64];

    ogs_assert(imsi);
    ogs_assert(out_imeisv);

    ogs_thread_mutex_lock(&self.lock);

    if (self.redis && redis_ensure() == OGS_OK) {
        redisReply *r = redisCommand(self.redis,
                "HGET %s imeisv", state_key(imsi, key, sizeof(key)));
        if (r && r->type == REDIS_REPLY_STRING) {
            *out_imeisv = ogs_strdup(r->str);
            rv = OGS_OK;
        }
        if (r) freeReplyObject(r);
        if (rv == OGS_OK) goto out;
    }

    state_entry_t *e = memory_get_or_create(imsi, false);
    if (e && e->imeisv) {
        *out_imeisv = ogs_strdup(e->imeisv);
        rv = OGS_OK;
    }
out:
    ogs_thread_mutex_unlock(&self.lock);
    return rv;
}

int ogs_flatfile_state_set_imeisv(const char *imsi, const char *imeisv)
{
    char key[64];

    ogs_assert(imsi);

    ogs_thread_mutex_lock(&self.lock);

    if (self.redis && redis_ensure() == OGS_OK) {
        redisReply *r = redisCommand(self.redis,
                "HSET %s imeisv %s",
                state_key(imsi, key, sizeof(key)),
                imeisv ? imeisv : "");
        if (r) freeReplyObject(r);
    }

    state_entry_t *e = memory_get_or_create(imsi, true);
    if (e->imeisv) ogs_free(e->imeisv);
    e->imeisv = imeisv ? ogs_strdup(imeisv) : NULL;

    ogs_thread_mutex_unlock(&self.lock);
    return OGS_OK;
}

/* -------- Remove (auto-reconcile when a SIM disappears from YAML) -------- */

int ogs_flatfile_state_remove(const char *imsi)
{
    char key[64];
    ogs_hash_index_t *hi;
    size_t imsi_len;

    ogs_assert(imsi);
    imsi_len = strlen(imsi);

    ogs_thread_mutex_lock(&self.lock);

    if (self.redis && redis_ensure() == OGS_OK) {
        redisReply *r = redisCommand(self.redis,
                "DEL %s", state_key(imsi, key, sizeof(key)));
        if (r) freeReplyObject(r);
    }

    /* Find the in-memory entry by iterating so we get the stored key
     * pointer (which we strdup'd in memory_get_or_create) and can free
     * both the entry and the key. ogs_hash_get only returns the value. */
    for (hi = ogs_hash_first(self.memory); hi; hi = ogs_hash_next(hi)) {
        const void *k;
        int klen;
        void *v;
        ogs_hash_this(hi, &k, &klen, &v);
        if ((size_t)klen == imsi_len && memcmp(k, imsi, klen) == 0) {
            state_entry_free((state_entry_t *)v);
            ogs_hash_set(self.memory, k, klen, NULL);
            ogs_free((void *)k);
            break;
        }
    }

    ogs_thread_mutex_unlock(&self.lock);
    return OGS_OK;
}
