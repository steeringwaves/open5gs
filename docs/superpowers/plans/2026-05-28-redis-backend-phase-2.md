# Redis Backend — Phase 2 Implementation Plan (reads + writes)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement a working Redis backend behind the `lib/dbi/` vtable that Phase 1 established: all subscriber/session/IMS *reads* plus the HSS/UDR *writes* (SQN, IMEISV, MME). A 4G/5G attach (and SQN update) works end-to-end against `db_uri: redis://...`. Only the change-stream/keyspace-notification watcher is deferred to Phase 3.

**Architecture:** A new `lib/dbi/redis/` backend registers the `redis`/`rediss` schemes via the existing `register_builtin_backends()` (gated by `OGS_DBI_HAVE_REDIS`). It connects with the hiredis sync API. Each subscriber is one JSON string at key `<prefix>subscriber:<supi-value>`; an MSISDN secondary index lives at `<prefix>msisdn:<bcd>`. Reads do `GET` + parse the JSON into the same C structs the MongoDB backend produces; the JSON shape mirrors the Mongo document 1:1 (same `lib/proto/types.h` key macros). Writes mutate the JSON field under optimistic locking (`WATCH`/`MULTI`/`EXEC`). cJSON is vendored into `lib/dbi/redis/` (self-contained; no libsbi dependency).

**Tech Stack:** C, meson/ninja, hiredis (libhiredis-dev, installed), vendored cJSON (MIT, single-file), ABTS test framework, Docker (for the optional equivalence/integration test).

**Prerequisites:** `libhiredis-dev` is installed (`pkg-config --modversion hiredis` → 1.2.0). The Phase 1 branch state is in place: `lib/dbi/ogs-dbi-backend.h` vtable, `ogs-dbi-core.c` registry + `register_builtin_backends()`, `OGS_DBI_HAVE_REDIS` already defined by `meson.build` when hiredis is found.

**Scope:** Phase 2 only. Phase 3 (keyspace-notification watcher → change events) and Phase 4 (`open5gs-dbctl` CLI) and Phase 5 (docs/packaging/CI) get their own plans. The Redis `watch_init`/`poll_change_stream` vtable slots are implemented as graceful "not supported in Phase 2" stubs here.

**Design decisions carried from brainstorming/spec (do not relitigate):**
- JSON: **vendored cJSON** in `lib/dbi/redis/` (not jansson, not libsbi's cJSON).
- Atomic field updates: **WATCH/MULTI** retry loop (no Lua).
- Key namespace prefix configurable via `db_uri` query string `?prefix=` (default `open5gs:`).
- SUPI handling: Redis keys use the **value only** (strip the `imsi-`/`imsi` type prefix that `ogs_id_get_type_value` splits out).
- The JSON document shape is identical to the Mongo document (so `mongoexport` → `redis-cli SET` migrates cleanly, addressed by the Phase 4 CLI).

**Build directory:** Reuse `build-phase1` (already configured). After any `meson.build` change run `meson setup build-phase1 --reconfigure`. NEVER `rm -rf` the build dir. Commit prefix: `dbi(redis):`.

---

## Design: the parse/IO split (read every reader this way)

Every read method is split into two functions so the parsing logic is unit-testable against a JSON fixture string with **no live Redis**:

- `redis_parse_<thing>(const cJSON *doc, <out-struct> *out)` — pure: walks an already-parsed cJSON document into the C struct. Mirrors the corresponding `mongoc-<file>.c` BSON walker 1:1, with the cJSON substitutions in the translation table below. Unit-tested directly.
- `redis_<thing>(...)` (the vtable method) — thin: builds the key, `GET`s the string from Redis, `cJSON_Parse`s it, calls `redis_parse_<thing>`, frees the cJSON and the reply.

### BSON-iter → cJSON translation table (apply mechanically when porting a walker)

| MongoDB (mongoc-*.c)                                   | cJSON (redis-*.c)                                                        |
|--------------------------------------------------------|--------------------------------------------------------------------------|
| iterate top-level: `bson_iter_init`+`bson_iter_next`   | `cJSON_ArrayForEach(item, doc)` (doc is the object; iterates members)    |
| `bson_iter_key(&it)`                                   | `item->string`                                                           |
| `bson_iter_recurse(&it, &child)` on a document         | `cJSON *child = item;` then `cJSON_GetObjectItemCaseSensitive(child,key)` or `cJSON_ArrayForEach` over its members |
| `BSON_ITER_HOLDS_UTF8(&it)`                             | `cJSON_IsString(item) && item->valuestring`                              |
| `BSON_ITER_HOLDS_INT32`/`INT64`                        | `cJSON_IsNumber(item)`                                                   |
| `BSON_ITER_HOLDS_BOOL`                                 | `cJSON_IsBool(item)`                                                     |
| `BSON_ITER_HOLDS_ARRAY`/`HOLDS_DOCUMENT`               | `cJSON_IsArray(item)` / `cJSON_IsObject(item)`                          |
| `bson_iter_utf8(&it, &len)`                            | `item->valuestring` (NUL-terminated; use `strlen` for length)            |
| `bson_iter_int32(&it)` / `bson_iter_int64(&it)`        | `(int)cJSON_GetNumberValue(item)` / `(int64_t)cJSON_GetNumberValue(item)`|
| `bson_iter_bool(&it)`                                  | `cJSON_IsTrue(item)`                                                     |
| recurse into named child doc                           | `cJSON *c = cJSON_GetObjectItemCaseSensitive(parent, KEY)`               |
| iterate array of docs                                  | `cJSON *el; cJSON_ArrayForEach(el, arrayItem) { ... }`                   |

**Field-key macros:** reuse the exact `OGS_*_STRING` macros from `lib/proto/types.h` (e.g. `OGS_SECURITY_STRING`, `OGS_K_STRING`, `OGS_OPC_STRING`, `OGS_AMF_STRING`, `OGS_SQN_STRING`, `OGS_AMBR_STRING`, `OGS_DOWNLINK_STRING`, `OGS_UPLINK_STRING`, `OGS_VALUE_STRING`, `OGS_UNIT_STRING`, `OGS_SLICE_STRING`, `OGS_SST_STRING`, `OGS_SD_STRING`, `OGS_DEFAULT_INDICATOR_STRING`, `OGS_SESSION_STRING`, `OGS_NAME_STRING`, `OGS_TYPE_STRING`, `OGS_QOS_STRING`, `OGS_INDEX_STRING`, `OGS_ARP_STRING`, `OGS_MSISDN_STRING`, `OGS_IMSI_STRING`, etc.). The Mongo reader already uses these as BSON keys; the JSON keys are identical.

**Number handling caution:** cJSON stores all numbers as `double` in `valuedouble` and an `int` snapshot in `valueint`. For values that can exceed 32 bits (notably `sqn`, a `uint64_t`), DO NOT use `valueint`. Use `(uint64_t)cJSON_GetNumberValue(item)` (reads `valuedouble`). SQN fits in 2^48 (well within double's 53-bit exact-integer range), so this is exact. This is called out again in the auth_info and update_sqn tasks.

---

## File Map

**Created:**
- `lib/dbi/redis/cJSON.h` — vendored verbatim from `lib/sbi/openapi/external/cJSON.h`
- `lib/dbi/redis/cJSON.c` — vendored verbatim from `lib/sbi/openapi/external/cJSON.c`
- `lib/dbi/redis/redis-internal.h` — internal types/decls for the redis backend (the `ogs_redis_t` connection state, the parse-function prototypes, `extern const ogs_dbi_backend_t redis_backend;`)
- `lib/dbi/redis/redis-backend.c` — vtable literal; `redis_init`/`redis_final` (URI parse + hiredis connect); a shared `redis_command`/reconnect helper; the WATCH/MULTI field-update helper; the watch stubs
- `lib/dbi/redis/redis-subscription.c` — `redis_parse_auth_info` + `redis_auth_info`; `redis_parse_subscription_data` + `redis_subscription_data`; the four writers (`redis_update_sqn`/`redis_increment_sqn`/`redis_update_imeisv`/`redis_update_mme`)
- `lib/dbi/redis/redis-session.c` — `redis_parse_session_data` + `redis_session_data`
- `lib/dbi/redis/redis-ims.c` — `redis_parse_msisdn_data` + `redis_msisdn_data`; `redis_parse_ims_data` + `redis_ims_data`
- `tests/dbi/redis-parse-test.c` — ABTS unit tests for the pure `redis_parse_*` functions against JSON fixtures (NO live Redis)
- `tests/dbi/fixtures/subscriber.json` — a canonical subscriber JSON fixture used by the parse tests

**Modified:**
- `lib/dbi/ogs-dbi-core.c` — `register_builtin_backends()`: add the `#ifdef OGS_DBI_HAVE_REDIS` registration of `redis_backend`
- `lib/dbi/meson.build` — under `if libhiredis_dep.found()`, add the `redis/*.c` sources (incl. vendored cJSON) and the redis include dir
- `tests/dbi/meson.build` — add `redis-parse-test.c`; install the fixtures dir
- `tests/dbi/abts-main.c` — register the `test_redis_parse` suite

**Out of scope (Phase 3+):** keyspace notifications, the `open5gs-dbctl` CLI, docs/packaging, `rediss://` TLS (parse-and-reject for now).

---

## Task 1: Vendor cJSON into `lib/dbi/redis/`

**Files:** Create `lib/dbi/redis/cJSON.h`, `lib/dbi/redis/cJSON.c`.

- [ ] **Step 1: Copy the vendored files**
```bash
mkdir -p lib/dbi/redis
cp lib/sbi/openapi/external/cJSON.h lib/dbi/redis/cJSON.h
cp lib/sbi/openapi/external/cJSON.c lib/dbi/redis/cJSON.c
```

- [ ] **Step 2: Add a provenance note** at the very top of BOTH copied files (above the existing cJSON MIT license header), as a one-line C comment:
```c
/* Vendored from lib/sbi/openapi/external/cJSON.c (cJSON, MIT). Kept here so the
 * Redis dbi backend does not depend on libsbi. Sync if the upstream copy changes. */
```
Do NOT modify any cJSON code itself.

- [ ] **Step 3: Verify the copies are byte-identical to the source (besides the note)**
```bash
diff <(tail -n +2 lib/dbi/redis/cJSON.c) lib/sbi/openapi/external/cJSON.c | head
```
Expected: the only difference is the added provenance line (diff shows the inserted note region). The cJSON body is unchanged.

- [ ] **Step 4: Commit**
```bash
git add lib/dbi/redis/cJSON.h lib/dbi/redis/cJSON.c
git commit -m "dbi(redis): vendor cJSON for self-contained JSON parsing"
```

---

## Task 2: Redis backend skeleton — connection, URI parse, registration, stubs

**Files:**
- Create: `lib/dbi/redis/redis-internal.h`, `lib/dbi/redis/redis-backend.c`
- Modify: `lib/dbi/ogs-dbi-core.c`, `lib/dbi/meson.build`
- Test: `tests/dbi/redis-parse-test.c` (URI-parse unit tests; created here, extended later)

This task gets a `redis://` backend that connects and registers, with every vtable method as a graceful stub, so `ogs_dbi_init("redis://...")` works and later tasks fill in real behavior.

- [ ] **Step 1: Write `lib/dbi/redis/redis-internal.h`** (full AGPL header, then):
```c
#ifndef OGS_DBI_REDIS_INTERNAL_H
#define OGS_DBI_REDIS_INTERNAL_H

#include "ogs-dbi-backend.h"
#include "redis/cJSON.h"

#include <hiredis/hiredis.h>

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
```

- [ ] **Step 2: Write the URI-parse failing test** in `tests/dbi/redis-parse-test.c` (full AGPL header, then). This test exercises a parse helper we expose for testing; we will add more cases in later tasks.
```c
#define OGS_DBI_COMPILATION
#include "ogs-dbi.h"
#include "ogs-dbi-backend.h"
#include "redis/redis-internal.h"
#include "core/abts.h"

/* redis_parse_uri is exposed (non-static) for testing; see redis-backend.c */
int redis_parse_uri(const char *uri, char **host, int *port, char **prefix);

static void uri_basic(abts_case *tc, void *data)
{
    char *host = NULL, *prefix = NULL; int port = 0;
    int rv = redis_parse_uri("redis://127.0.0.1:6380/?prefix=net:",
                             &host, &port, &prefix);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);
    ABTS_STR_EQUAL(tc, "127.0.0.1", host);
    ABTS_INT_EQUAL(tc, 6380, port);
    ABTS_STR_EQUAL(tc, "net:", prefix);
    ogs_free(host); ogs_free(prefix);
}

static void uri_defaults(abts_case *tc, void *data)
{
    char *host = NULL, *prefix = NULL; int port = 0;
    int rv = redis_parse_uri("redis://localhost", &host, &port, &prefix);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);
    ABTS_STR_EQUAL(tc, "localhost", host);
    ABTS_INT_EQUAL(tc, OGS_REDIS_DEFAULT_PORT, port);
    ABTS_STR_EQUAL(tc, OGS_REDIS_DEFAULT_PREFIX, prefix);
    ogs_free(host); ogs_free(prefix);
}

static void uri_rejects_non_redis(abts_case *tc, void *data)
{
    char *host = NULL, *prefix = NULL; int port = 0;
    int rv = redis_parse_uri("mongodb://localhost/open5gs", &host, &port, &prefix);
    ABTS_INT_EQUAL(tc, OGS_ERROR, rv);
}

abts_suite *test_redis_parse(abts_suite *suite);
abts_suite *test_redis_parse(abts_suite *suite)
{
    suite = ADD_SUITE(suite);
    abts_run_test(suite, uri_basic, NULL);
    abts_run_test(suite, uri_defaults, NULL);
    abts_run_test(suite, uri_rejects_non_redis, NULL);
    return suite;
}
```
Register the suite in `tests/dbi/abts-main.c` (`abts_suite *test_redis_parse(abts_suite *suite);` + `{ test_redis_parse },` in `alltests[]`), and add `redis-parse-test.c` to `tests/dbi/meson.build`'s source list.

- [ ] **Step 3: Write `lib/dbi/redis/redis-backend.c`** (full AGPL header, then). Implement: `redis_parse_uri` (non-static, for tests), `redis_init`/`redis_final`, `ogs_redis()`, `redis_subscriber_key`, `redis_get_string`, `redis_update_subscriber`, the `redis_backend` vtable literal, and Phase-2 stubs for `redis_watch_init`/`redis_poll_change_stream`. The reader/writer methods are declared in redis-internal.h and defined in the other files (Tasks 3-7); for THIS task, to keep the build linking, also add temporary `ogs_fatal`+`return OGS_ERROR` stubs for `redis_auth_info`, `redis_subscription_data`, `redis_session_data`, `redis_msisdn_data`, `redis_ims_data`, `redis_update_sqn`, `redis_increment_sqn`, `redis_update_imeisv`, `redis_update_mme` at the bottom of redis-backend.c (each removed as its real impl lands in later tasks).

URI parser (handles `redis://host[:port][/db][?prefix=...]`; rejects non-`redis`/`rediss` schemes; `rediss` accepted as scheme but TLS not yet wired — log a warning and treat as plain for Phase 2... actually REJECT `rediss://` with a clear "TLS not supported until a later phase" error to avoid silently-insecure connections):
```c
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
```

`redis_init`/`redis_final`/`ogs_redis`:
```c
static ogs_redis_t self;

ogs_redis_t *ogs_redis(void) { return &self; }

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
    if (self.ctx) { redisFree(self.ctx); self.ctx = NULL; }
    if (self.host) { ogs_free(self.host); self.host = NULL; }
    if (self.prefix) { ogs_free(self.prefix); self.prefix = NULL; }
    if (self.masked_uri) { ogs_free(self.masked_uri); self.masked_uri = NULL; }
    self.initialized = false;
}
```

`redis_subscriber_key` and `redis_get_string`:
```c
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
```

`redis_update_subscriber` (WATCH/MULTI optimistic-lock retry):
```c
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
        if (reply) freeReplyObject(reply);
        reply = redisCommand(ogs_redis()->ctx, "SET %s %s", key, serialized);
        if (reply) freeReplyObject(reply);
        free(serialized);   /* cJSON_PrintUnformatted uses malloc, not ogs_malloc */

        reply = redisCommand(ogs_redis()->ctx, "EXEC");
        if (!reply) goto done;
        /* EXEC returns nil array on WATCH conflict -> retry */
        if (reply->type == REDIS_REPLY_NIL ||
                (reply->type == REDIS_REPLY_ARRAY && reply->elements == 0)) {
            freeReplyObject(reply);
            ogs_warn("[%s] redis update conflict, retry %d", supi, attempt + 1);
            continue;
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
```
> NOTE the `free(serialized)` (libc free) vs `ogs_free` — `cJSON_PrintUnformatted` allocates with the allocator cJSON was built with (libc malloc by default). Use `cJSON_free(serialized)` if available in the vendored copy; otherwise `free()`. Verify which by checking the vendored `cJSON.h` for `cJSON_free`. Prefer `cJSON_free`.

Vtable + watch stubs:
```c
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
    ogs_warn("Redis change notifications are not implemented yet (Phase 3); "
             "subscriber edits will not propagate to a running HSS.");
    return OGS_ERROR;
}
int redis_poll_change_stream(void) { return OGS_ERROR; }
```
Plus the temporary `ogs_fatal` stubs for the 9 read/write methods (removed in Tasks 3-7).

- [ ] **Step 4: Register the redis backend in `lib/dbi/ogs-dbi-core.c`**
In `register_builtin_backends()`, after the mongoc block, add:
```c
#ifdef OGS_DBI_HAVE_REDIS
    ogs_dbi_backend_register(&redis_backend);
#endif
```
and near the top, after the mongoc extern:
```c
#ifdef OGS_DBI_HAVE_REDIS
extern const ogs_dbi_backend_t redis_backend;
#endif
```

- [ ] **Step 5: meson — add the redis sources** in `lib/dbi/meson.build`. Change the `if libhiredis_dep.found()` block to add the sources and the redis include dir:
```meson
libhiredis_dep = dependency('hiredis', required: get_option('redis'))
if libhiredis_dep.found()
    libdbi_sources += files('''
        redis/cJSON.h
        redis/cJSON.c
        redis/redis-internal.h
        redis/redis-backend.c
        redis/redis-subscription.c
        redis/redis-session.c
        redis/redis-ims.c
    '''.split())
    backend_deps += libhiredis_dep
    backend_c_args += '-DOGS_DBI_HAVE_REDIS'
endif
```
And add `'redis'` to `libdbi_inc`: `libdbi_inc = include_directories('.', 'mongoc', 'redis')`.
NOTE: redis-subscription.c / redis-session.c / redis-ims.c don't exist until Tasks 3-7. To keep this task's build green, CREATE them now as stub files containing only the full AGPL header + `#include "redis/redis-internal.h"` (no functions yet — the methods are stubbed in redis-backend.c). Tasks 3-7 move the stubs out of redis-backend.c into these files as real impls.

- [ ] **Step 6: Build + run URI tests**
```
meson setup build-phase1 --reconfigure && ninja -C build-phase1 && meson test -C build-phase1 --suite dbi -v
```
Expected: clean build (redis backend compiled + registered); the 3 URI tests pass; existing dbi/unit tests still pass.

- [ ] **Step 7: Smoke-test registration** — confirm `ogs_dbi_init("redis://...")` selects the redis backend. Add one ABTS case to `redis-parse-test.c`:
```c
static void init_selects_redis_backend(abts_case *tc, void *data)
{
    /* No server needed: redis_init returns OGS_RETRY when it can't connect,
     * but ogs_dbi_init only sets `active` on OGS_OK. So assert the backend is
     * FOUND in the registry instead. */
    ABTS_PTR_NOTNULL(tc, (void *)ogs_dbi_backend_find("redis"));
}
```
(Register it in `test_redis_parse`.)

- [ ] **Step 8: Commit**
```bash
git add lib/dbi/redis/redis-internal.h lib/dbi/redis/redis-backend.c \
        lib/dbi/redis/redis-subscription.c lib/dbi/redis/redis-session.c \
        lib/dbi/redis/redis-ims.c lib/dbi/ogs-dbi-core.c lib/dbi/meson.build \
        tests/dbi/redis-parse-test.c tests/dbi/abts-main.c tests/dbi/meson.build
git commit -m "dbi(redis): backend skeleton with hiredis connection, URI parse, registration"
```

---

## Task 3: `auth_info` reader (parse/IO split, TDD)

**Files:** `lib/dbi/redis/redis-subscription.c` (move out of stub), `tests/dbi/redis-parse-test.c`, `tests/dbi/fixtures/subscriber.json`.

Reference: `lib/dbi/mongoc/mongoc-subscription.c:mongoc_auth_info` (the BSON walker for the `security` sub-document: K/OPC/OP/AMF/RAND as hex strings → bytes via `ogs_ascii_to_hex`, SQN as int64).

- [ ] **Step 1: Create the fixture `tests/dbi/fixtures/subscriber.json`** — a canonical subscriber mirroring the Mongo document. Include at least: `imsi`, `security` {k, opc, amf, rand, sqn}, `msisdn` array, `ambr` {downlink/uplink {value,unit}}, one `slice` with one `session` (name, type, qos{index,arp{...}}, ambr), `access_restriction_data`, `subscriber_status`. (Use realistic 32-hex-char K/OPC, e.g. `"465B5CE8B199B49FAA5F0A2EE238A6BC"`.)

- [ ] **Step 2: Write the failing parse test** in `redis-parse-test.c`:
```c
static cJSON *load_fixture(void)
{
    char *path = ogs_msprintf("%s/tests/dbi/fixtures/subscriber.json",
            /* test working dir is the build dir; use the source dir via env or
             * a relative path from the repo root. Simplest: embed the JSON as a
             * string constant instead of reading a file. */ ".");
    /* See note below — prefer an embedded JSON string constant for hermeticity. */
    ...
}
```
**Decision:** to avoid test-cwd fragility, embed the fixture JSON as a `static const char SUBSCRIBER_JSON[] = "...";` string constant in the test file rather than reading the file. (Keep the `fixtures/subscriber.json` file too, for the equivalence test in Task 8.) The test:
```c
static void parse_auth_info_ok(abts_case *tc, void *data)
{
    cJSON *doc = cJSON_Parse(SUBSCRIBER_JSON);
    ogs_dbi_auth_info_t auth;
    int rv;
    ABTS_PTR_NOTNULL(tc, doc);
    rv = redis_parse_auth_info(doc, &auth);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);
    ABTS_INT_EQUAL(tc, 1, auth.use_opc);            /* fixture sets opc */
    /* K first byte 0x46 from "465B..." */
    ABTS_INT_EQUAL(tc, 0x46, auth.k[0]);
    ABTS_TRUE(tc, auth.sqn > 0);
    cJSON_Delete(doc);
}
```

- [ ] **Step 3: Run it — expect link failure** (`redis_parse_auth_info` undefined / still a stub). `ninja -C build-phase1 && meson test -C build-phase1 --suite dbi -v` → fail.

- [ ] **Step 4: Implement `redis_parse_auth_info` + `redis_auth_info` in `lib/dbi/redis/redis-subscription.c`**, porting `mongoc_auth_info`'s `security` walk using the translation table. Complete code:
```c
#include "redis/redis-internal.h"

int redis_parse_auth_info(const cJSON *doc, ogs_dbi_auth_info_t *auth_info)
{
    cJSON *security, *item;
    char buf[OGS_KEY_LEN];

    ogs_assert(doc); ogs_assert(auth_info);
    memset(auth_info, 0, sizeof(*auth_info));

    security = cJSON_GetObjectItemCaseSensitive((cJSON *)doc, OGS_SECURITY_STRING);
    if (!security || !cJSON_IsObject(security)) {
        ogs_error("No '" OGS_SECURITY_STRING "' in subscriber doc");
        return OGS_ERROR;
    }

    cJSON_ArrayForEach(item, security) {
        const char *key = item->string;
        if (!key || !cJSON_IsString(item) ) {
            /* sqn is a number; handle below */
        }
        if (!strcmp(key, OGS_K_STRING) && cJSON_IsString(item)) {
            ogs_ascii_to_hex(item->valuestring, strlen(item->valuestring),
                    buf, sizeof(buf));
            memcpy(auth_info->k, buf, OGS_KEY_LEN);
        } else if (!strcmp(key, OGS_OPC_STRING) && cJSON_IsString(item)) {
            auth_info->use_opc = 1;
            ogs_ascii_to_hex(item->valuestring, strlen(item->valuestring),
                    buf, sizeof(buf));
            memcpy(auth_info->opc, buf, OGS_KEY_LEN);
        } else if (!strcmp(key, OGS_OP_STRING) && cJSON_IsString(item)) {
            ogs_ascii_to_hex(item->valuestring, strlen(item->valuestring),
                    buf, sizeof(buf));
            memcpy(auth_info->op, buf, OGS_KEY_LEN);
        } else if (!strcmp(key, OGS_AMF_STRING) && cJSON_IsString(item)) {
            ogs_ascii_to_hex(item->valuestring, strlen(item->valuestring),
                    buf, sizeof(buf));
            memcpy(auth_info->amf, buf, OGS_AMF_LEN);
        } else if (!strcmp(key, OGS_RAND_STRING) && cJSON_IsString(item)) {
            ogs_ascii_to_hex(item->valuestring, strlen(item->valuestring),
                    buf, sizeof(buf));
            memcpy(auth_info->rand, buf, OGS_RAND_LEN);
        } else if (!strcmp(key, OGS_SQN_STRING) && cJSON_IsNumber(item)) {
            auth_info->sqn = (uint64_t)cJSON_GetNumberValue(item);  /* NOT valueint */
        }
    }
    return OGS_OK;
}

int redis_auth_info(char *supi, ogs_dbi_auth_info_t *auth_info)
{
    char *key, *json;
    cJSON *doc;
    int rv;

    ogs_assert(supi); ogs_assert(auth_info);
    key = redis_subscriber_key(supi);
    if (!key) return OGS_ERROR;
    json = redis_get_string(key);
    ogs_free(key);
    if (!json) { ogs_info("[%s] Cannot find IMSI in DB", supi); return OGS_ERROR; }
    doc = cJSON_Parse(json);
    ogs_free(json);
    if (!doc) { ogs_error("[%s] malformed subscriber JSON", supi); return OGS_ERROR; }
    rv = redis_parse_auth_info(doc, auth_info);
    cJSON_Delete(doc);
    return rv;
}
```
Remove the temporary `redis_auth_info` stub from `redis-backend.c`.

- [ ] **Step 5: Run — expect pass.** `ninja -C build-phase1 && meson test -C build-phase1 --suite dbi -v` → the parse tests pass.

- [ ] **Step 6: Commit**
```bash
git add lib/dbi/redis/redis-subscription.c lib/dbi/redis/redis-backend.c \
        tests/dbi/redis-parse-test.c tests/dbi/fixtures/subscriber.json tests/dbi/meson.build
git commit -m "dbi(redis): implement auth_info reader (cJSON parse) with unit tests"
```

---

## Task 4: `subscription_data` reader (port the large walker, TDD)

**Files:** `lib/dbi/redis/redis-subscription.c`, `tests/dbi/redis-parse-test.c`.

Reference: `lib/dbi/mongoc/mongoc-subscription.c:mongoc_subscription_data` (lines ~308-838) — the big nested walker covering: `msisdn[]`, `imsi`, `access_restriction_data`, `subscriber_status`, `operator_determined_barring`, `network_access_mode`, `subscribed_rau_tau_timer`, `ambr{downlink/uplink{value,unit}}` with the unit-scaling `for (n=0;n<unit;n++) *=1000`, `slice[]` → each {sst, sd, default_indicator, session[]} → each session {name, type, lbo_roaming_allowed, qos{index, arp{priority_level, pre_emption_capability, pre_emption_vulnerability}}, ambr{...}, smf{ipv4,ipv6}, ue{ipv4,ipv6}, ipv4_framed_routes[], ipv6_framed_routes[]}, plus `mme_host`/`mme_realm`/`purge_flag`.

- [ ] **Step 1: Write failing tests** asserting the parsed `ogs_subscription_data_t` from `SUBSCRIBER_JSON`: `num_of_msisdn`, `ambr.downlink/uplink` (post unit-scaling), `num_of_slice`, `slice[0].s_nssai.sst`, `slice[0].num_of_session`, `slice[0].session[0].name`, `session[0].qos.index`, `session[0].qos.arp.priority_level`. Add cases for a multi-slice fixture variant too (embed a second JSON constant with 2 slices / 2 sessions to exercise the array loops).

- [ ] **Step 2: Run — expect fail.**

- [ ] **Step 3: Implement `redis_parse_subscription_data` + `redis_subscription_data`.** Port `mongoc_subscription_data` 1:1 using the translation table. WORKED PATTERNS (apply throughout):

*AMBR with unit-scaling* (recurs in subscription and session):
```c
static void parse_bitrate(const cJSON *obj, uint64_t *out)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive((cJSON *)obj, OGS_VALUE_STRING);
    cJSON *u = cJSON_GetObjectItemCaseSensitive((cJSON *)obj, OGS_UNIT_STRING);
    uint8_t unit = (u && cJSON_IsNumber(u)) ? (uint8_t)cJSON_GetNumberValue(u) : 0;
    int n;
    *out = (v && cJSON_IsNumber(v)) ? (uint64_t)cJSON_GetNumberValue(v) : 0;
    for (n = 0; n < unit; n++) *out *= 1000;
}
```
*array-of-objects loop* (slice[], session[]):
```c
cJSON *slice_arr = cJSON_GetObjectItemCaseSensitive((cJSON *)doc, OGS_SLICE_STRING);
if (slice_arr && cJSON_IsArray(slice_arr)) {
    cJSON *slice_el;
    cJSON_ArrayForEach(slice_el, slice_arr) {
        ogs_assert(subscription_data->num_of_slice < OGS_MAX_NUM_OF_SLICE);
        ogs_slice_data_t *slice_data =
            &subscription_data->slice[subscription_data->num_of_slice];
        /* ... fill sst/sd/default_indicator/session[] ... */
        subscription_data->num_of_slice++;
    }
}
```
*SD parse:* `slice_data->s_nssai.sd = ogs_s_nssai_sd_from_string(sd->valuestring);` (same as mongoc). Default `sd.v = OGS_S_NSSAI_NO_SD_VALUE`, `sst=0` before filling; skip the slice with `ogs_error("No SST")` if sst absent (mirror mongoc's `sst_presence`).
*framed routes:* same alloc pattern as mongoc (`ogs_calloc(OGS_MAX_NUM_OF_FRAMED_ROUTES_IN_PDI, ...)`, fill from the string array, free-existing-first guard).
*IP fields:* `ogs_ipsubnet(&ipsub, valuestring, NULL)` then set `smf_ip`/`ue_ip` exactly as mongoc.

The function is a structural mirror of the mongoc walker; keep the SAME order, SAME bounds asserts (`OGS_MAX_NUM_OF_*`), SAME field handling. The equivalence test (Task 8) is the correctness gate.

`redis_subscription_data` (thin wrapper) — identical shape to `redis_auth_info`'s wrapper (GET → parse → call `redis_parse_subscription_data` → cleanup). Remove the stub from redis-backend.c.

- [ ] **Step 4: Run — expect pass.**
- [ ] **Step 5: Commit** `dbi(redis): implement subscription_data reader`.

---

## Task 5: `session_data` reader (TDD)

**Files:** `lib/dbi/redis/redis-session.c`, `tests/dbi/redis-parse-test.c`.

Reference: `lib/dbi/mongoc/mongoc-session.c:mongoc_session_data` — walks `slice[]`→`session[]` like subscription_data but SELECTS the session matching `s_nssai` (sst+sd) and `dnn` (session name), filling a single `ogs_session_data_t`.

- [ ] **Step 1: Failing test** — `redis_parse_session_data(doc, &s_nssai, "internet", &out)` returns OGS_OK and fills `out.session.name == "internet"`, qos, ambr; and returns OGS_ERROR for a non-existent DNN. (s_nssai built from the fixture's sst/sd.)
- [ ] **Step 2: Run — fail.**
- [ ] **Step 3: Implement** `redis_parse_session_data` (+ thin `redis_session_data` wrapper). Port the mongoc matching logic: iterate slices, match `sst` (+ `sd` when present), iterate that slice's sessions, match `name == dnn`, then fill the session (reuse the AMBR/QoS/IP patterns from Task 4). Return OGS_ERROR if no match (mirror mongoc's `found` flag). Remove the stub.
- [ ] **Step 4: Run — pass.**
- [ ] **Step 5: Commit** `dbi(redis): implement session_data reader`.

---

## Task 6: `msisdn_data` + `ims_data` readers (TDD)

**Files:** `lib/dbi/redis/redis-ims.c`, `tests/dbi/redis-parse-test.c`.

References: `mongoc-ims.c:mongoc_msisdn_data` (imsi + msisdn[] → bcd buffers) and `mongoc_ims_data` (msisdn[] + the big `ifc[]` walk: priority, application_server{server_name, default_handling}, trigger_point{condition_type_cnf, spt[]{condition_negated, group, method/session_case/sip_header{header,content}/sdp_line{line,content}/request_uri, with the per-type `OGS_SPT_HAS_*` tagging}}).

### MSISDN secondary-index design
`redis_msisdn_data(id, ...)` must resolve by IMSI **or** MSISDN (mongoc used `$or [imsi, msisdn]`). Redis approach:
1. `GET <prefix>msisdn:<id>` → if it returns a string, that's the IMSI; build subscriber key from it.
2. Else treat `id` as the IMSI/value directly: `GET <prefix>subscriber:<id>`.
3. Parse the resulting doc with `redis_parse_msisdn_data`.
(The index keys are written by the Phase 4 CLI; for Phase 2 tests, the equivalence-test harness/provisioner sets them.)

- [ ] **Step 1: Failing tests** — `redis_parse_msisdn_data(doc, &out)` fills `out.imsi.bcd` and `out.msisdn[0].bcd`; `redis_parse_ims_data(doc, &out)` fills `num_of_msisdn` and (with an `ifc`-bearing fixture variant) `num_of_ifc`, `ifc[0].priority`, an SPT entry's type/method. Embed an `IMS_JSON` fixture string with one `ifc` containing one `spt` of `method` type.
- [ ] **Step 2: Run — fail.**
- [ ] **Step 3: Implement** both parse functions (port the mongoc walkers) + thin wrappers. `redis_msisdn_data` does the index lookup described above (add a small `redis_get_string` on the msisdn key, fall back to subscriber key). Remove the stubs.
- [ ] **Step 4: Run — pass.**
- [ ] **Step 5: Commit** `dbi(redis): implement msisdn_data and ims_data readers`.

---

## Task 7: Writers — SQN / increment_sqn / IMEISV / MME (WATCH/MULTI, TDD)

**Files:** `lib/dbi/redis/redis-subscription.c`, `tests/dbi/redis-parse-test.c` (mutate-fn unit tests).

References: `mongoc-subscription.c`: `mongoc_update_sqn` (`$set security.sqn`), `mongoc_increment_sqn` (`$inc security.sqn` by 32 then `$bit and OGS_MAX_SQN`), `mongoc_update_imeisv` (`$set imeisv` upsert), `mongoc_update_mme` (`$set mme_host/mme_realm/mme_timestamp/purge_flag`).

The writers use `redis_update_subscriber(supi, mutate_fn, data)` from Task 2. Each mutate fn edits the parsed cJSON doc. The mutate functions are PURE (operate on a cJSON doc) → unit-testable without Redis.

- [ ] **Step 1: Failing tests** for the mutate functions against a parsed fixture doc:
  - `sqn_set_mutate(doc, &(uint64_t){42})` → `security.sqn == 42`.
  - `sqn_increment_mutate(doc, NULL)` → `security.sqn == old + 32` masked with `OGS_MAX_SQN`.
  - `imeisv_set_mutate(doc, "1234...")` → top-level `imeisv` set.
  - `mme_set_mutate(doc, &struct{host,realm,purge})` → `mme_host`/`mme_realm`/`purge_flag` set, `mme_timestamp` present.
  Use `cJSON`-based assertions to read back the mutated values.
- [ ] **Step 2: Run — fail.**
- [ ] **Step 3: Implement** the four mutate functions + the four vtable methods that call `redis_update_subscriber`. Worked example (SQN set):
```c
static int sqn_set_mutate(cJSON *doc, void *data)
{
    uint64_t sqn = *(uint64_t *)data;
    cJSON *sec = cJSON_GetObjectItemCaseSensitive(doc, OGS_SECURITY_STRING);
    if (!sec) { sec = cJSON_AddObjectToObject(doc, OGS_SECURITY_STRING); }
    cJSON_DeleteItemFromObjectCaseSensitive(sec, OGS_SQN_STRING);
    cJSON_AddNumberToObject(sec, OGS_SQN_STRING, (double)sqn);
    return OGS_OK;
}
int redis_update_sqn(char *supi, uint64_t sqn)
{
    return redis_update_subscriber(supi, sqn_set_mutate, &sqn);
}
```
`increment_sqn`: read current `security.sqn` (0 if absent), `+= 32`, `&= OGS_MAX_SQN`, set. `update_imeisv`: set top-level `imeisv` string (cJSON: delete-then-add). `update_mme`: set `mme_host`, `mme_realm`, `mme_timestamp` (= `ogs_time_now()` as number), `purge_flag` (bool). Mirror the exact field names/macros mongoc used. Remove the 4 stubs.
> NOTE on the Mongo schema quirk: `mme_host`/`mme_realm`/`imeisv`/`purge_flag` are stored as ARRAYS in the Mongoose schema (`[String]`/`[Boolean]`) per `webui/server/models/subscriber.js`, but `mongoc_update_mme` writes them as scalar `$set` fields. To preserve read parity with `mongoc_subscription_data` (which reads `mme_host` as UTF8 scalar — re-check the mongoc reader), match whatever the mongoc READER expects. Verify by reading `mongoc_subscription_data`'s handling of `OGS_MME_HOST_STRING` and store the SAME JSON type the reader will read back. (The reader treats it as a scalar UTF8 string.)
- [ ] **Step 4: Run — pass.**
- [ ] **Step 5: Commit** `dbi(redis): implement SQN/IMEISV/MME writers via WATCH/MULTI`.

---

## Task 8: Equivalence + round-trip integration test (Docker; optional gate)

**Files:** `tests/dbi/redis-equivalence-test.c` (new), `tests/dbi/meson.build`, a small test provisioner.

This is the load-bearing correctness check: load the SAME canonical subscriber into Redis (and optionally Mongo) and assert the read structs match.

- [ ] **Step 1: Add a Redis-backed test** that:
  - Skips (returns early, ABTS pass) if env `OGS_TEST_REDIS_URI` is unset (so the suite stays green without Docker).
  - If set: `ogs_dbi_init($OGS_TEST_REDIS_URI)`; provision the fixture via direct `redisCommand` SET of `SUBSCRIBER_JSON` at `<prefix>subscriber:001010000000001` and the msisdn index; then call `ogs_dbi_auth_info`/`ogs_dbi_subscription_data`/`ogs_dbi_session_data`/`ogs_dbi_msisdn_data`/`ogs_dbi_ims_data` and assert the same values the parse-unit-tests check; then exercise `ogs_dbi_update_sqn`/`increment_sqn` and re-read to confirm persistence; `ogs_dbi_final()`.
- [ ] **Step 2: Provide a helper script** `tests/dbi/run-redis-tests.sh` that spins up `redis:7-alpine` on a random port, exports `OGS_TEST_REDIS_URI=redis://127.0.0.1:<port>/?prefix=test:`, runs `meson test -C build-phase1 --suite dbi`, and tears the container down. Skips with a clear message if `docker` is absent.
- [ ] **Step 3: Run locally if Docker is available**; otherwise confirm the suite still passes (skip path). 
- [ ] **Step 4: Commit** `dbi(redis): add Docker-gated equivalence/round-trip test`.

---

## Task 9: Build-matrix + end-to-end smoke

- [ ] **Step 1: Matrix** — confirm in throwaway build dirs (keep `build-phase1`): `-Dmongo=enabled -Dredis=enabled` (default now) builds; `-Dmongo=disabled -Dredis=enabled` now BUILDS (redis-only — this is the new Phase 2 capability; verify libdbi links without libmongoc and the `mongodb` scheme is simply absent from the registry); `-Dmongo=disabled -Dredis=disabled` still errors.
- [ ] **Step 2: Leak/dep check** — `-Dmongo=disabled -Dredis=enabled`: confirm `libogsdbi.so` does NOT link `libmongoc` (`ldd build-XXX/lib/dbi/libogsdbi.so* | grep -i mongoc` empty) and DOES link `libhiredis`.
- [ ] **Step 3: Soft manual smoke** (document, optional): run an HSS or UDR against a local `redis:7` with one provisioned subscriber; observe a successful auth/attach and an SQN write landing in Redis (`redis-cli GET open5gs:subscriber:001010000000001`). This is the real proof the dispatch path works end-to-end; mark soft per spec section 8.9.
- [ ] **Step 4: Commit** any matrix-driven fixes; tag `phase-2-complete` locally (do not push without user go-ahead).

---

## Self-Review

**Spec coverage (spec sections 2-7):** reads (auth/subscription/session/msisdn/ims) = Tasks 3-6; writes (sqn/imeisv/mme) = Task 7 (pulled forward per the user's "reads+writes together" decision); data model + key layout + prefix = Task 2; WATCH/MULTI = Task 2+7; cJSON vendored = Task 1; meson gating + redis-only build = Task 2+9; equivalence test (Docker, skip-if-absent) = Task 8. Change notifications (spec §5.2) correctly DEFERRED to Phase 3 (watch stubs in Task 2). CLI (spec §6) deferred to Phase 4.

**Placeholder scan:** the large walkers (Tasks 4-6) are specified as faithful structural ports of named mongoc functions with a complete translation table + worked patterns + the equivalence test as the gate, rather than 1500 lines of pre-written C. This is deliberate: pasting the full ported walkers would duplicate the mongoc source and risk silent drift; the translation table makes the port mechanical and Task 8 verifies behavioral equivalence. All infrastructure (URI parse, connection, WATCH/MULTI, auth_info, the SQN writer) has complete code.

**Type/name consistency:** `ogs_redis_t`/`ogs_redis()`; `redis_parse_*` (pure) vs `redis_*` (vtable I/O); `redis_update_subscriber(supi, redis_mutate_f, data)`; `redis_backend` registered under `OGS_DBI_HAVE_REDIS`. Vtable method names match `ogs_dbi_backend_t` slots exactly. `(uint64_t)cJSON_GetNumberValue` for SQN (not `valueint`) is called out in Tasks 3 and 7.

**Open risks to watch during execution:**
1. cJSON number precision for SQN — use `cJSON_GetNumberValue` (double), not `valueint`. Covered.
2. `cJSON_PrintUnformatted` allocator — free with `cJSON_free` (verify it exists in the vendored header), not `ogs_free`. Covered in Task 2 note.
3. `mme_host`/`imeisv`/`purge_flag` scalar-vs-array schema quirk — Task 7 instructs matching the mongoc READER's expectation; verify against `mongoc_subscription_data` before writing.
4. WATCH/MULTI on a single shared `redisContext` — fine for the HSS (serialized by `self.db_lock`) and single-threaded NF init; revisit if a future NF calls dbi writes from multiple threads without external locking.
