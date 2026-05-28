# Redis Backend — Phase 3 Implementation Plan (change-notification watcher)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the Redis change-notification watcher so subscriber edits propagate to a running HSS (S6a IDR/CLR), reaching parity with MongoDB change streams. Phase 1 built the backend-neutral plumbing (`ogs_dbi_change_event_t`, `ogs_dbi_set_change_handler`, `ogs_dbi_dispatch_change_event`) and the HSS consumer; Phase 2 left `redis_watch_init`/`redis_poll_change_stream` as graceful stubs. This phase makes them real.

**Architecture (spec §5.2 — two-channel model):** A dedicated hiredis subscriber connection (separate from the GET/SET connection) listens on two sources:
1. **Rich channel** — `SUBSCRIBE <prefix>events:subscriber`. A future writer (the Phase 4 `open5gs-dbctl`) PUBLISHes `{"imsi":"...","fields":["ambr","slice",...]}`; the watcher maps the field names to `OGS_DBI_FIELD_*` bits for a precise change event.
2. **Keyspace-notification fallback** — `PSUBSCRIBE __keyspace@*__:<prefix>subscriber:*`. Catches any third-party writer (`redis-cli`, scripts). The payload only names the event (`set`/`del`), so the watcher emits `OGS_DBI_FIELD_ALL` (HSS sends a full IDR — correct, just chattier). A short suppression window dedupes a keyspace event against a rich event for the same IMSI.

The HSS already registers its handler in `hss_db_watch_init()` and pumps the poll via its timer (under `self.db_lock`), all backend-agnostic. **No HSS code changes are needed** — the redis watcher plugs into the existing vtable slots and the callback.

**Tech Stack:** C, hiredis pub/sub (sync connection, non-blocking drain), vendored cJSON (already in `lib/dbi/redis/`), ABTS, Docker (live test).

**Prerequisites:** Phases 1+2 complete (tags `phase-1-complete`, `phase-2-complete`). Build dir `build-phase1` (mongo+redis enabled). hiredis 1.2.0, Docker 29.x available.

**Decisions carried from brainstorming (do not relitigate):**
- The HSS enable flag stays `use_mongodb_change_stream` (reused as-is for Redis; documented). No new/renamed flag.
- Implement BOTH channels now (rich + keyspace fallback). The rich-channel publisher arrives in Phase 4; Phase 3 tests it with synthetic `PUBLISH`es.
- `redis.conf` must have keyspace notifications enabled; the watcher best-effort `CONFIG SET notify-keyspace-events Kg$` at init and warns (does not fail) if denied.

**Build directory:** reuse `build-phase1`; `meson setup build-phase1 --reconfigure` after meson edits; NEVER delete it. Commit prefix `dbi(redis):`.

---

## Design notes

### Connection & non-blocking drain
`redis_watch_init` opens a SECOND `redisContext *sub_ctx` (the GET/SET `ctx` cannot be used for pub/sub — once subscribed, a connection only receives messages). After SUBSCRIBE/PSUBSCRIBE, set `sub_ctx->fd` non-blocking (`fcntl O_NONBLOCK`). `redis_poll_change_stream` (called periodically by the HSS timer) drains all buffered messages without blocking:
```c
/* non-blocking drain */
if (redisBufferRead(sub_ctx) == REDIS_ERR) {
    if (sub_ctx->err == REDIS_ERR_IO &&
            (errno == EAGAIN || errno == EWOULDBLOCK)) {
        /* no data this tick — not an error */
        sub_ctx->err = 0; sub_ctx->errstr[0] = '\0';
    } else {
        /* real error → attempt reconnect on next watch tick */
        return OGS_ERROR;
    }
}
void *vreply;
while (redisGetReplyFromReader(sub_ctx, &vreply) == REDIS_OK && vreply) {
    redisReply *reply = vreply;
    process_message(reply);   /* see below */
    freeReplyObject(reply);
}
```

### pub/sub message shapes (hiredis)
- `SUBSCRIBE`/`PSUBSCRIBE` confirmations arrive as arrays `["subscribe", channel, count]` — ignore.
- A rich-channel message: `["message", "<prefix>events:subscriber", "<json payload>"]` → element[2] is the JSON.
- A keyspace message: `["pmessage", "<pattern>", "__keyspace@0__:<prefix>subscriber:<imsi>", "<event>"]` → element[2] is the channel naming the key; element[3] is the event name (`set`/`del`/…).

`process_message` distinguishes by element[0] (`"message"` vs `"pmessage"`) and the channel.

### Keyspace channel → IMSI
Channel = `__keyspace@<db>__:<prefix>subscriber:<imsi>`. Strip the leading `__keyspace@N__:` (find the first `:` after `@`), then strip the known `<prefix>subscriber:` head (the watcher knows `ogs_redis()->prefix`); the remainder is the bare IMSI value. Return NULL if the channel doesn't match the expected shape.

### Field-name → OGS_DBI_FIELD_* (rich channel)
Map the strings in `"fields"` (same names HSS cares about) to bits. Mirrors the inverse of the HSS consumer:
| field name (substring match, as mongoc used) | bit |
|---|---|
| `request_cancel_location` | `OGS_DBI_FIELD_REQUEST_CANCEL_LOCATION` |
| `access_restriction_data` | `OGS_DBI_FIELD_ACCESS_RESTRICTION_DATA` |
| `subscriber_status` | `OGS_DBI_FIELD_SUBSCRIBER_STATUS` |
| `operator_determined_barring` | `OGS_DBI_FIELD_OP_DETERMINED_BARRING` |
| `network_access_mode` | `OGS_DBI_FIELD_NETWORK_ACCESS_MODE` |
| `ambr` | `OGS_DBI_FIELD_AMBR` |
| `subscribed_rau_tau_timer` | `OGS_DBI_FIELD_RAU_TAU_TIMER` |
| `slice` | `OGS_DBI_FIELD_SLICE` |
Unknown field names → contribute nothing (or, if the `"fields"` array is absent/empty, default to `OGS_DBI_FIELD_ALL` so a vaguely-formed rich event still triggers a refresh).

### Suppression window (dedupe keyspace vs rich)
Both channels fire for one CLI write (the CLI does `SET` → keyspace `set` event AND `PUBLISH`es the rich event). Keep a small ring buffer of recent rich IMSIs + timestamps (`OGS_REDIS_SUPPRESS_SLOTS = 16`). On a rich event: dispatch precise + record `(imsi, now)`. On a keyspace event: if that IMSI appears in the ring within `OGS_REDIS_SUPPRESS_WINDOW` (≈200 ms), skip it; else dispatch `OGS_DBI_FIELD_ALL`. Best-effort: a same-tick keyspace-before-rich race may yield one redundant `ALL` IDR (correct, just chattier) — acceptable.

---

## File Map

**Modified:**
- `lib/dbi/redis/redis-internal.h` — add `sub_ctx` + suppression ring to `ogs_redis_t`; add pure-helper prototypes; constants.
- `lib/dbi/redis/redis-backend.c` — implement `redis_watch_init`/`redis_poll_change_stream` (replace the Phase-2 stubs); close `sub_ctx` in `redis_final`.
- `lib/dbi/redis/redis-watch.c` — **NEW**: the pure helpers (`redis_event_field_from_name`, `redis_parse_rich_event`, `redis_keyspace_channel_to_imsi`, suppression check) + `process_message`. (Keeping the watch logic in its own file mirrors `mongoc-watch.c` and keeps `redis-backend.c` focused on connection/IO.)
- `lib/dbi/meson.build` — add `redis/redis-watch.c` to the redis source list.
- `tests/dbi/redis-parse-test.c` — unit tests for the pure helpers.
- `tests/dbi/redis-equivalence-test.c` — extend with a live watcher test (keyspace + rich) guarded by `OGS_TEST_REDIS_URI`.
- `tests/dbi/run-redis-tests.sh` — ensure keyspace notifications are enabled in the test container.
- `configs/open5gs/hss.yaml.in` — comment documenting that `use_mongodb_change_stream: true` also enables Redis change notifications, and that `redis.conf` needs `notify-keyspace-events`.
- `docs/_docs/...` (optional, light) — note in the redis backend guidance.

**No changes to** `src/hss/*` (the watcher plugs into the existing vtable + callback).

---

## Task 1: Connection plumbing — `sub_ctx`, watch_init, final

**Files:** `lib/dbi/redis/redis-internal.h`, `lib/dbi/redis/redis-backend.c`.

- [ ] **Step 1: Extend `ogs_redis_t` and add constants/prototypes in `redis-internal.h`.** Add to the struct:
```c
    redisContext *sub_ctx;   /* dedicated pub/sub connection for watch */
    struct {
        char imsi_bcd[OGS_MAX_IMSI_BCD_LEN + 1];
        ogs_time_t when;
    } suppress[16];
    int suppress_next;       /* ring write index */
```
Add constants:
```c
#define OGS_REDIS_SUPPRESS_SLOTS 16
#define OGS_REDIS_SUPPRESS_WINDOW ogs_time_from_msec(200)
#define OGS_REDIS_EVENTS_CHANNEL_SUFFIX "events:subscriber"
```
Add prototypes (implemented in redis-watch.c):
```c
uint32_t redis_event_field_from_name(const char *name);
int redis_parse_rich_event(const char *json, char **imsi_out, uint32_t *mask_out);
char *redis_keyspace_channel_to_imsi(const char *channel);
void redis_watch_process_message(redisReply *reply);
```

- [ ] **Step 2: Write a failing watch_init test** (in redis-equivalence-test.c or a new lightweight case) — actually, watch_init needs a live server; the meaningful unit tests are the pure helpers (Task 2). For Task 1, the verification is: build green + the existing dbi suite still passes + a Docker smoke that `redis_watch_init` connects and CONFIG-SETs without error. Defer the live assertion to Task 4. For TDD discipline here, write the helper-less part by building and running existing tests.

- [ ] **Step 3: Implement `redis_watch_init` in `redis-backend.c`** (replace the Phase-2 stub):
```c
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
```
(Add `#include <fcntl.h>` and `#include <errno.h>` to redis-backend.c.)

- [ ] **Step 4: Close `sub_ctx` in `redis_final`.** Add at the top of `redis_final`:
```c
    if (self.sub_ctx) { redisFree(self.sub_ctx); self.sub_ctx = NULL; }
```

- [ ] **Step 5: Temporary poll** — keep `redis_poll_change_stream` returning OGS_OK as a no-op for now (real drain lands in Task 3), so the build links:
```c
int redis_poll_change_stream(void) { return OGS_OK; }
```

- [ ] **Step 6: Build + test.** `meson setup build-phase1 --reconfigure && ninja -C build-phase1 && meson test -C build-phase1 --suite dbi -v` → clean, all pass.

- [ ] **Step 7: Commit** `dbi(redis): open dedicated pub/sub connection and subscribe in watch_init`.

---

## Task 2: Pure watch helpers + unit tests (TDD)

**Files:** create `lib/dbi/redis/redis-watch.c`; modify `lib/dbi/meson.build`, `tests/dbi/redis-parse-test.c`.

- [ ] **Step 1: Write failing unit tests** in `redis-parse-test.c`:
  - `redis_event_field_from_name("ambr")` == `OGS_DBI_FIELD_AMBR`; `"slice"` == `OGS_DBI_FIELD_SLICE`; `"subscriber_status"` == `OGS_DBI_FIELD_SUBSCRIBER_STATUS`; unknown `"nope"` == 0.
  - `redis_parse_rich_event("{\"imsi\":\"001010000000001\",\"fields\":[\"ambr\",\"slice\"]}", &imsi, &mask)` → OGS_OK, imsi=="001010000000001", mask == (AMBR|SLICE). Free imsi.
  - `redis_parse_rich_event` with no `fields` → mask == OGS_DBI_FIELD_ALL.
  - `redis_parse_rich_event` with no `imsi` → OGS_ERROR.
  - `redis_keyspace_channel_to_imsi("__keyspace@0__:open5gs:subscriber:001010000000001")` (with prefix "open5gs:") → "001010000000001". Non-matching channel → NULL. (NOTE: the test must run with a known prefix; either set `ogs_redis()->prefix` via a tiny test setter or have the helper take the prefix as a param. PREFER making the helper take prefix as a param: `redis_keyspace_channel_to_imsi(const char *channel, const char *prefix)` — cleaner and test-friendly. Update the prototype accordingly.)
  Register the cases in the suite.

- [ ] **Step 2: Run — red** (link failure: helpers undefined).

- [ ] **Step 3: Implement `lib/dbi/redis/redis-watch.c`** (full AGPL header, then):
```c
#include "redis/redis-internal.h"

uint32_t redis_event_field_from_name(const char *name)
{
    if (!name) return 0;
    if (!strcmp(name, "request_cancel_location"))
        return OGS_DBI_FIELD_REQUEST_CANCEL_LOCATION;
    if (!strncmp(name, OGS_ACCESS_RESTRICTION_DATA_STRING,
            strlen(OGS_ACCESS_RESTRICTION_DATA_STRING)))
        return OGS_DBI_FIELD_ACCESS_RESTRICTION_DATA;
    if (!strncmp(name, OGS_SUBSCRIBER_STATUS_STRING,
            strlen(OGS_SUBSCRIBER_STATUS_STRING)))
        return OGS_DBI_FIELD_SUBSCRIBER_STATUS;
    if (!strncmp(name, OGS_OPERATOR_DETERMINED_BARRING_STRING,
            strlen(OGS_OPERATOR_DETERMINED_BARRING_STRING)))
        return OGS_DBI_FIELD_OP_DETERMINED_BARRING;
    if (!strncmp(name, OGS_NETWORK_ACCESS_MODE_STRING,
            strlen(OGS_NETWORK_ACCESS_MODE_STRING)))
        return OGS_DBI_FIELD_NETWORK_ACCESS_MODE;
    if (!strncmp(name, "ambr", strlen("ambr")))
        return OGS_DBI_FIELD_AMBR;
    if (!strncmp(name, OGS_SUBSCRIBED_RAU_TAU_TIMER_STRING,
            strlen(OGS_SUBSCRIBED_RAU_TAU_TIMER_STRING)))
        return OGS_DBI_FIELD_RAU_TAU_TIMER;
    if (!strncmp(name, "slice", strlen("slice")))
        return OGS_DBI_FIELD_SLICE;
    return 0;
}

int redis_parse_rich_event(const char *json, char **imsi_out, uint32_t *mask_out)
{
    cJSON *doc, *imsi, *fields;
    uint32_t mask = 0;

    ogs_assert(imsi_out); ogs_assert(mask_out);
    *imsi_out = NULL; *mask_out = 0;

    doc = cJSON_Parse(json);
    if (!doc) return OGS_ERROR;

    imsi = cJSON_GetObjectItemCaseSensitive(doc, "imsi");
    if (!imsi || !cJSON_IsString(imsi) || !imsi->valuestring) {
        cJSON_Delete(doc);
        return OGS_ERROR;
    }
    *imsi_out = ogs_strdup(imsi->valuestring);

    fields = cJSON_GetObjectItemCaseSensitive(doc, "fields");
    if (fields && cJSON_IsArray(fields) && cJSON_GetArraySize(fields) > 0) {
        cJSON *f;
        cJSON_ArrayForEach(f, fields) {
            if (cJSON_IsString(f) && f->valuestring)
                mask |= redis_event_field_from_name(f->valuestring);
        }
        if (mask == 0) mask = OGS_DBI_FIELD_ALL; /* unknown fields → refresh all */
    } else {
        mask = OGS_DBI_FIELD_ALL;               /* no field list → refresh all */
    }
    *mask_out = mask;

    cJSON_Delete(doc);
    return OGS_OK;
}

char *redis_keyspace_channel_to_imsi(const char *channel, const char *prefix)
{
    const char *p, *head_end;
    char *head;

    if (!channel || !prefix) return NULL;
    /* skip "__keyspace@<db>__:" → find the ':' that ends that segment */
    p = strchr(channel, ':');
    if (!p) return NULL;
    p++;  /* now at "<prefix>subscriber:<imsi>" */

    head = ogs_msprintf("%ssubscriber:", prefix);
    ogs_assert(head);
    if (strncmp(p, head, strlen(head)) != 0) {
        ogs_free(head);
        return NULL;
    }
    head_end = p + strlen(head);
    ogs_free(head);
    if (*head_end == '\0') return NULL;
    return ogs_strdup(head_end);
}
```
(`process_message` + the suppression check land in Task 3, which also defines the suppression helpers.)

- [ ] **Step 4: Add `redis/redis-watch.c` to `lib/dbi/meson.build`** (in the `if libhiredis_dep.found()` source list).

- [ ] **Step 5: Run — green.** Build + dbi suite pass.

- [ ] **Step 6: Commit** `dbi(redis): add pure change-event parsing helpers with unit tests`.

---

## Task 3: `redis_poll_change_stream` drain + dispatch + suppression

**Files:** `lib/dbi/redis/redis-watch.c` (process_message + suppression), `lib/dbi/redis/redis-backend.c` (poll drain).

- [ ] **Step 1: Add suppression helpers + `redis_watch_process_message` to `redis-watch.c`:**
```c
static bool suppress_recent_rich(const char *imsi_bcd)
{
    int i;
    ogs_time_t now = ogs_time_now();
    for (i = 0; i < OGS_REDIS_SUPPRESS_SLOTS; i++) {
        if (ogs_redis()->suppress[i].imsi_bcd[0] &&
                !strcmp(ogs_redis()->suppress[i].imsi_bcd, imsi_bcd) &&
                (now - ogs_redis()->suppress[i].when) < OGS_REDIS_SUPPRESS_WINDOW)
            return true;
    }
    return false;
}

static void remember_rich(const char *imsi_bcd)
{
    int idx = ogs_redis()->suppress_next % OGS_REDIS_SUPPRESS_SLOTS;
    ogs_cpystrn(ogs_redis()->suppress[idx].imsi_bcd, imsi_bcd,
            sizeof(ogs_redis()->suppress[idx].imsi_bcd));
    ogs_redis()->suppress[idx].when = ogs_time_now();
    ogs_redis()->suppress_next++;
}

void redis_watch_process_message(redisReply *reply)
{
    if (!reply || reply->type != REDIS_REPLY_ARRAY || reply->elements < 3)
        return;

    const char *kind = reply->element[0] ? reply->element[0]->str : NULL;
    if (!kind) return;

    if (!strcmp(kind, "message")) {
        /* rich channel: element[2] = JSON payload */
        if (reply->elements < 3 || !reply->element[2] ||
                reply->element[2]->type != REDIS_REPLY_STRING)
            return;
        char *imsi = NULL; uint32_t mask = 0;
        if (redis_parse_rich_event(reply->element[2]->str, &imsi, &mask)
                == OGS_OK && imsi) {
            remember_rich(imsi);
            ogs_dbi_dispatch_change_event(
                    ogs_dbi_change_event_alloc(imsi, mask));
            ogs_free(imsi);
        }
    } else if (!strcmp(kind, "pmessage")) {
        /* keyspace fallback: element[2] = channel naming the key */
        if (reply->elements < 4 || !reply->element[2] ||
                reply->element[2]->type != REDIS_REPLY_STRING)
            return;
        char *imsi = redis_keyspace_channel_to_imsi(
                reply->element[2]->str, ogs_redis()->prefix);
        if (!imsi) return;
        if (!suppress_recent_rich(imsi)) {
            ogs_dbi_dispatch_change_event(
                    ogs_dbi_change_event_alloc(imsi, OGS_DBI_FIELD_ALL));
        }
        ogs_free(imsi);
    }
    /* "subscribe"/"psubscribe" confirmations: ignored */
}
```
Note: `ogs_dbi_dispatch_change_event` hands the event to the HSS-registered handler (Phase 1). `ogs_dbi_change_event_alloc(NULL,...)` returns NULL and dispatch tolerates NULL, so the imsi-present guard is belt-and-suspenders.

- [ ] **Step 2: Implement the non-blocking drain in `redis_poll_change_stream` (redis-backend.c):**
```c
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
```

- [ ] **Step 3: Build + dbi suite pass** (no live server needed for the build; helpers already unit-tested).

- [ ] **Step 4: Commit** `dbi(redis): drain pub/sub and dispatch neutral change events`.

---

## Task 4: Live watcher integration test (Docker)

**Files:** `tests/dbi/redis-equivalence-test.c`, `tests/dbi/run-redis-tests.sh`.

- [ ] **Step 1: Ensure keyspace events are on in the test container.** In `run-redis-tests.sh`, after the container is up, run `docker exec $cid redis-cli CONFIG SET notify-keyspace-events Kg$` (the backend also tries, but set it explicitly so the test is deterministic).

- [ ] **Step 2: Add a watcher test** to `redis-equivalence-test.c` (runs only when `OGS_TEST_REDIS_URI` set; otherwise skip). Pattern:
  - Register a capture handler via `ogs_dbi_set_change_handler(capture_cb, &captured)` where `capture_cb` copies the event's `imsi_bcd` + `updated_fields_mask` into a test-local struct and frees the event.
  - `ogs_dbi_init($OGS_TEST_REDIS_URI)`; `ogs_dbi_collection_watch_init()` (→ redis_watch_init). Give the subscription a moment (a few short `ogs_dbi_poll_change_stream()` calls with `ogs_msleep(50)` between, to consume the SUBSCRIBE confirmations).
  - **Keyspace path:** from a second `redisContext` (or `docker exec redis-cli`), `SET <prefix>subscriber:001010000000001 <json>`. Then loop `ogs_dbi_poll_change_stream()` + `ogs_msleep(50)` up to ~1 s until `captured.imsi_bcd` is set. Assert it equals `001010000000001` and mask == `OGS_DBI_FIELD_ALL`.
  - **Rich path:** reset capture; `PUBLISH <prefix>events:subscriber {"imsi":"001010000000001","fields":["ambr","slice"]}` from the second connection; poll until captured; assert mask == `(AMBR|SLICE)`.
  - `ogs_dbi_final()`; `ogs_dbi_set_change_handler(NULL, NULL)`.
  - Skip cleanly (ABTS pass + log) if `OGS_TEST_REDIS_URI` unset.

- [ ] **Step 3: Run live** (`sh tests/dbi/run-redis-tests.sh`) — confirm both the keyspace (FIELD_ALL) and rich (precise mask) paths deliver events through the handler. Capture output. Confirm the no-env run still skips cleanly.

- [ ] **Step 4: Commit** `dbi(redis): live watcher test for keyspace + rich change events`.

---

## Task 5: HSS-against-Redis smoke, docs, build matrix, tag

- [ ] **Step 1: Config doc.** In `configs/open5gs/hss.yaml.in`, update the comment near `use_mongodb_change_stream` to note it ALSO enables Redis change notifications when `db_uri` is `redis://`, and that the Redis server needs `notify-keyspace-events` (the backend best-effort-sets it). Keep the flag name as-is.

- [ ] **Step 2: Build matrix.** Confirm (throwaway dirs, keep build-phase1): default `-Dmongo=enabled -Dredis=enabled` builds + dbi/unit pass; `-Dmongo=disabled -Dredis=enabled` full build still succeeds (the watcher adds no mongoc dependency).

- [ ] **Step 3: Soft manual smoke (document, optional).** Run `open5gs-hssd` with `use_mongodb_change_stream: true` against a live `redis:7` (keyspace events enabled); provision a subscriber; `redis-cli SET` a change to `subscriber_status`; observe the HSS log an IDR/CLR. Mark soft per spec §8.9.

- [ ] **Step 4: Commit** docs (`dbi(redis): document change-notification config`) and tag `phase-3-complete` locally (do NOT push without user go-ahead).

---

## Self-Review

**Spec coverage (spec §5.2):** rich `events:subscriber` channel = Task 2+3; keyspace `__keyspace@*__:...subscriber:*` fallback = Task 2+3; `CONFIG SET notify-keyspace-events` best-effort = Task 1; suppression dedupe = Task 3; field-name→bit mapping = Task 2; delivery via the Phase-1 callback to HSS = Task 3 (`ogs_dbi_dispatch_change_event`). HSS unchanged (decision: reuse `use_mongodb_change_stream`) = documented in Task 5.

**Placeholder scan:** complete code for watch_init, the drain, all three pure helpers, process_message, and suppression. The live-test body is described step-by-step against concrete fixtures (the same `001010000000001` subscriber used in Phase 2's equivalence test).

**Type/name consistency:** `ogs_redis()->sub_ctx`; helpers `redis_event_field_from_name`/`redis_parse_rich_event`/`redis_keyspace_channel_to_imsi(channel, prefix)`/`redis_watch_process_message`; constants `OGS_REDIS_SUPPRESS_SLOTS/WINDOW`, `OGS_REDIS_EVENTS_CHANNEL_SUFFIX`. The watcher emits via `ogs_dbi_change_event_alloc` + `ogs_dbi_dispatch_change_event` (Phase-1 API). Vtable slots `watch_init`/`poll_change_stream` already wired in Phase 2.

**Risks to watch during execution:**
1. **hiredis non-blocking drain semantics** — `redisBufferRead` + `redisGetReplyFromReader` is the correct non-blocking idiom; the EAGAIN clear (resetting `sub_ctx->err`) is required or the context goes into a sticky error state. Verify against the installed hiredis 1.2.0 headers.
2. **Reconnect** — if `sub_ctx` errors (server restart), Phase 3 returns OGS_ERROR from poll; a full reconnect/re-subscribe loop is a refinement. Acceptable for Phase 3 (HSS keeps polling; document that a Redis restart needs an HSS restart to re-subscribe, or add reconnect if cheap).
3. **Prefix with regex-special chars in PSUBSCRIBE** — the keyspace PSUBSCRIBE pattern interpolates `prefix`; if a prefix contained glob metacharacters it could mis-match. Default `open5gs:` is safe; note the constraint.
4. **db_lock** — poll runs under the HSS `self.db_lock` (Phase 1 shim), and `sub_ctx` is only touched by watch_init/poll/final on the HSS thread, so no extra locking needed.
