# Redis backend for open5gs lib/dbi

**Status:** Draft — design approved 2026-05-26, awaiting written-spec review.
**Owner:** 1800alex@gmail.com
**Target branch:** `metrics-main` (or `main`)

## 1. Motivation

MongoDB is the only supported subscriber-data backend in open5gs today. The
`mongod` daemon is heavy: ~100–200 MB resident on a small system before any
real load. On a Raspberry Pi class device, that dominates the memory budget and
makes open5gs awkward to deploy as a self-contained 5G core.

This spec adds a Redis backend as a runtime-selectable alternative. Redis is
~3 MB resident, single-threaded, packaged everywhere, and offers the two
primitives we need: a key-value store and a publish/subscribe mechanism that
maps cleanly onto MongoDB's change streams.

A flat-JSON-file backend was considered and rejected: the running NFs (HSS,
UDR) write back to the subscriber record for SQN, IMEISV, and MME tracking,
which makes file-based persistence (locking, self-inotify suppression,
atomic-rename semantics) materially more complex than the value justifies on a
single-Pi setup. Redis is fast enough.

## 2. Goals and non-goals

### Goals

- Add `redis://` as a `db_uri` scheme alongside the existing `mongodb://`.
- Backend chosen at runtime from the URI scheme; no callers in `src/` need to
  know which backend is active.
- Both backends gated by meson options. At least one must be enabled.
- Full feature parity for subscriber/session/MSISDN/IMS data and HSS-style
  change notifications.
- Provide a small C CLI tool, `open5gs-dbctl`, for provisioning the Redis
  backend. Pi users get a usable workflow without the Node.js webui.
- Existing MongoDB users see no functional change.

### Non-goals (v1)

- Webui (Node.js / Mongoose) is not updated to talk to Redis. It stays
  MongoDB-only. Pi users use `open5gs-dbctl` instead.
- No flat-file / SQLite / LMDB backend.
- No Redis Cluster or Sentinel support — single-node only.
- No automated `migrate-from-mongo` subcommand; the migration recipe is
  documented (`mongoexport` → `open5gs-dbctl import`).
- No subscriber-data encryption at rest in Redis.
- No per-NF authorization to subscriber subsets.
- TLS to Redis (`rediss://`) is plumbed if straightforward via hiredis;
  otherwise deferred.

## 3. Architecture

### 3.1 Backend abstraction (vtable in `lib/dbi/`)

A struct of function pointers is populated at `ogs_dbi_init(uri)` time by
parsing the URI scheme. The existing public `ogs_dbi_*` API in
`subscription.h` / `session.h` / `ims.h` / `ogs-mongoc.h` is unchanged; its
implementation becomes a thin dispatcher to the active backend.

```c
/* lib/dbi/ogs-dbi-backend.h (new, internal) */
typedef struct ogs_dbi_backend_s {
    const char *name;      /* "mongoc", "redis" */
    const char *scheme;    /* "mongodb", "redis" */

    int  (*init)(const char *uri);
    void (*final)(void);

    int  (*auth_info)(char *supi, ogs_dbi_auth_info_t *out);
    int  (*update_sqn)(char *supi, uint64_t sqn);
    int  (*increment_sqn)(char *supi);
    int  (*update_imeisv)(char *supi, char *imeisv);
    int  (*update_mme)(char *supi, char *host, char *realm, bool purge);

    int  (*subscription_data)(char *supi, ogs_subscription_data_t *out);
    int  (*session_data)(const char *supi, const ogs_s_nssai_t *s_nssai,
                         const char *dnn, ogs_session_data_t *out);
    int  (*msisdn_data)(char *id, ogs_msisdn_data_t *out);
    int  (*ims_data)(char *supi, ogs_ims_data_t *out);

    /* change notification — NULL means "not supported on this backend" */
    int  (*watch_init)(void);
    int  (*poll_change_stream)(void);
} ogs_dbi_backend_t;
```

`ogs_dbi_init(uri)` parses the scheme, looks it up in a built-in registry,
calls `backend->init(uri)`, and stashes the pointer in a static. The thin
wrappers in `subscription.c` look like:

```c
int ogs_dbi_auth_info(char *supi, ogs_dbi_auth_info_t *out) {
    return ogs_dbi_current_backend()->auth_info(supi, out);
}
```

### 3.2 File layout

```
lib/dbi/
├── ogs-dbi.h               (umbrella header, unchanged public API)
├── ogs-dbi-backend.h       (NEW: internal vtable + registration)
├── ogs-dbi-core.c          (NEW: ogs_dbi_init/final, scheme dispatch)
├── subscription.h, session.h, ims.h, ogs-mongoc.h   (unchanged)
├── subscription.c, session.c, ims.c                 (REWRITTEN: dispatchers)
├── ogs-mongoc.c            (kept; ogs_mongoc() accessor stays for compat)
├── mongoc/
│   ├── mongoc-backend.c           (registers "mongodb" scheme)
│   ├── mongoc-subscription.c      (BSON-walking code from current subscription.c)
│   ├── mongoc-session.c
│   ├── mongoc-ims.c
│   └── mongoc-watch.c             (existing change-streams code)
└── redis/
    ├── redis-backend.c            (registers "redis"/"rediss" schemes)
    ├── redis-subscription.c
    ├── redis-session.c
    ├── redis-ims.c
    └── redis-watch.c              (keyspace notifications)
```

### 3.3 Backwards compatibility

- `ogs_dbi_*` public functions: signatures unchanged.
- `ogs_mongoc_t` / `ogs_mongoc()` accessor: kept. Code that imports
  `ogs-mongoc.h` directly (out-of-tree) still compiles and links when the
  Mongo backend is enabled.
- Default config files (`configs/*.yaml.in`) keep
  `db_uri: mongodb://localhost/open5gs` — no behavior change for existing
  users.
- Webui: no changes. Continues to write to MongoDB only.

## 4. Redis data model

### 4.1 Key namespace

All keys are prefixed `open5gs:`. The prefix is configurable via the
`db_uri` query string, e.g. `redis://localhost/?prefix=mynet:`.

### 4.2 Primary record

```
KEY    open5gs:subscriber:<imsi-or-supi-value>
TYPE   string  (JSON)
```

The JSON shape **mirrors the existing MongoDB document 1:1**. Same field
names, same nesting. This is deliberate: it makes `mongoexport` →
`redis-cli SET` a trivial migration, and it lets the JSON-walking code be
ported directly from the BSON-walking code in `subscription.c` / `ims.c` with
field-by-field translation.

Example:

```json
{
  "schema_version": 1,
  "imsi": "001010000000001",
  "msisdn": ["1234567890"],
  "imeisv": ["..."],
  "mme_host": ["..."],
  "mme_realm": ["..."],
  "purge_flag": [false],
  "security": {
    "k": "...", "opc": "...", "amf": "...", "rand": "...", "sqn": 96
  },
  "ambr": {
    "downlink": {"value": 1, "unit": 3},
    "uplink":   {"value": 1, "unit": 3}
  },
  "slice": [ /* same shape as Mongo */ ],
  "access_restriction_data": 32,
  "subscriber_status": 0,
  "operator_determined_barring": 0,
  "network_access_mode": 0,
  "subscribed_rau_tau_timer": 12,
  "ifc": [ /* same shape as Mongo */ ]
}
```

### 4.3 SUPI-type handling

The MongoDB code splits SUPI strings (e.g. `imsi-001010000000001`) with
`ogs_id_get_type_value` into a type (`imsi`) and a value (`001010000000001`)
and queries on the typed field. Redis primary keys use the **value only**;
Redis keys are strings, not typed fields, so the type distinction is
irrelevant on the storage side. Callers continue to pass typed SUPI strings;
the Redis backend strips the prefix internally.

### 4.4 Secondary index — MSISDN → IMSI

```
KEY    open5gs:msisdn:<msisdn-bcd>
TYPE   string
VALUE  "<imsi>"
```

`ogs_dbi_msisdn_data(id, ...)`:

1. `GET open5gs:msisdn:<id>` → if non-empty, treat as IMSI.
2. Else treat `id` as IMSI directly.
3. `GET open5gs:subscriber:<imsi>` and parse.

Two round trips worst case. On local-loopback Redis this is sub-millisecond.

Secondary index keys are maintained by `open5gs-dbctl` on add/update/delete.
The running NFs never write MSISDN-index keys.

### 4.5 Atomic field updates

For HSS/UDR-driven writes (`update_sqn`, `increment_sqn`, `update_imeisv`,
`update_mme`):

- Implementation: `WATCH key` → `GET key` → mutate JSON in C → `MULTI` /
  `SET key newjson` / `EXEC`. Retry on `EXEC` returning nil (conflict),
  with bounded attempts (e.g. 5) and a logged warning on giveup.
- No Lua scripting. Keeps the dependency surface to plain hiredis.
- Single-NF Pi setup has effectively zero contention, so the retry loop
  rarely fires.

### 4.6 JSON library

Reuse `ogs_json` (jansson-based) already pulled into open5gs for SBI message
handling. No new JSON dep.

### 4.7 Memory footprint

A typical subscriber JSON is 1–4 KB. 100 subscribers ≈ 200–400 KB of values
plus Redis overhead. Redis itself is ~3 MB RSS. MongoDB by comparison is
~100–200 MB just to start `mongod`.

## 5. Change notifications

### 5.0 Change-event payload abstraction (consumer-side change required)

The current HSS code is **not** backend-agnostic. `src/hss/hss-context.c`
calls `mongoc_change_stream_next(ogs_mongoc()->stream, ...)` directly,
plumbs a `bson_t *` through the HSS event bus (`e->dbi.document`), and
parses the BSON in `hss_handle_change_event(const bson_t *document)` to
build a `subdatamask` of changed fields. This must be hidden behind the
vtable so the Redis backend can participate.

**Neutral event struct** (new, in `ogs-dbi.h`):

```c
typedef enum {
    OGS_DBI_FIELD_REQUEST_CANCEL_LOCATION = 1 << 0,
    OGS_DBI_FIELD_ACCESS_RESTRICTION_DATA = 1 << 1,
    OGS_DBI_FIELD_SUBSCRIBER_STATUS       = 1 << 2,
    OGS_DBI_FIELD_OP_DETERMINED_BARRING   = 1 << 3,
    OGS_DBI_FIELD_NETWORK_ACCESS_MODE     = 1 << 4,
    OGS_DBI_FIELD_AMBR                    = 1 << 5,
    OGS_DBI_FIELD_RAU_TAU_TIMER           = 1 << 6,
    OGS_DBI_FIELD_SLICE                   = 1 << 7,
    /* Add bits as new HSS-watched fields are discovered. */
    OGS_DBI_FIELD_ALL                     = 0xFFFFFFFF,
} ogs_dbi_field_t;

typedef struct ogs_dbi_change_event_s {
    char     *imsi_bcd;            /* heap-allocated; freed via ogs_dbi_change_event_free */
    uint32_t  updated_fields_mask; /* bitmask of ogs_dbi_field_t */
} ogs_dbi_change_event_t;

void ogs_dbi_change_event_free(ogs_dbi_change_event_t *e);
```

**HSS call-site changes** (small, mechanical):

- `hss_handle_change_event(const bson_t *)` → `hss_handle_change_event(const ogs_dbi_change_event_t *)`.
- `e->dbi.document` (a `bson_t *`) → `e->dbi.change_event` (an `ogs_dbi_change_event_t *`).
- The inline BSON-walking loop at `hss-context.c:1432` is replaced by reading `event->imsi_bcd` and testing `event->updated_fields_mask` against `OGS_DBI_FIELD_*` bits to set `send_clr_flag` / `send_idr_flag` / `subdatamask`.
- The `bson_destroy(e->dbi.document)` call in `hss-sm.c:96` becomes `ogs_dbi_change_event_free(e->dbi.change_event)`.

**Backend responsibility:** each backend's `poll_change_stream` produces
`ogs_dbi_change_event_t` records and pushes them onto the same event bus
HSS already drains. The Mongo backend converts BSON to the neutral struct
internally; the Redis backend builds the struct from its own notification
source (see 5.2).

### 5.1 MongoDB (existing logic, relocated)

`ogs_dbi_collection_watch_init()` still opens a change stream on the
`subscribers` collection. `ogs_dbi_poll_change_stream()` is still called
from `src/hss/hss-timer.c` on a recurring timer. The BSON-walking code that
used to live inline in `hss-context.c` moves to `mongoc/mongoc-watch.c`,
where it produces an `ogs_dbi_change_event_t` (translating BSON field names
to `OGS_DBI_FIELD_*` bits) and emits it on the HSS event bus.

### 5.2 Redis

Redis keyspace notifications only tell us "key X was SET/DEL" — they do
not carry the changed fields. To populate `updated_fields_mask`, the Redis
backend uses a **two-channel approach**:

1. **Rich event channel** (preferred, used by `open5gs-dbctl` and any
   Redis-aware writer): the writer `PUBLISH`es a small JSON payload to
   `open5gs:events:subscriber` whenever it updates a subscriber:

   ```json
   { "imsi": "001010000000001",
     "fields": ["ambr", "slice", "subscriber_status"] }
   ```

   The watcher subscribes to this channel and translates field names to
   `OGS_DBI_FIELD_*` bits with high fidelity. The CLI publishes this on
   every write it performs (add / del / update). HSS-driven internal writes
   (SQN, IMEISV, MME tracking) do **not** publish here — those changes are
   not S6a-relevant and would just create noise.

2. **Keyspace-notification fallback** (catches third-party writers using
   `redis-cli`, scripts, or future tools that don't know about the rich
   channel): the watcher also `PSUBSCRIBE`s
   `__keyspace@*__:open5gs:subscriber:*`. On any event whose key was *not*
   just covered by a rich-channel event in the last ~100 ms, the watcher
   emits an `ogs_dbi_change_event_t` with `updated_fields_mask =
   OGS_DBI_FIELD_ALL`. HSS sends an IDR for every field — correct but
   chattier. Acceptable for the rare hand-edit case.

A dedicated hiredis connection is used for both subscriptions (separate
from the connection used for normal GET/SET).

```c
/* redis-watch.c — sketch */
static redisContext *sub_ctx;

int redis_watch_init(void) {
    sub_ctx = redisConnect(host, port);
    /* Best-effort: ensure server has keyspace events enabled.
       Fails silently on managed Redis with locked-down CONFIG. */
    redisReply *r = redisCommand(sub_ctx,
        "CONFIG SET notify-keyspace-events Kg$");
    if (!r || r->type == REDIS_REPLY_ERROR) {
        ogs_warn("Redis CONFIG SET notify-keyspace-events denied; "
                 "set 'notify-keyspace-events Kg$' in redis.conf manually "
                 "or HSS subscriber-updated propagation will not work.");
    }
    /* Rich-channel subscription (preferred — carries updated-field list). */
    redisCommand(sub_ctx, "SUBSCRIBE %s:events:subscriber", prefix);
    /* Keyspace-notification fallback (covers third-party writers). */
    redisCommand(sub_ctx,
        "PSUBSCRIBE __keyspace@*__:%s:subscriber:*", prefix);
    /* set socket non-blocking for poll model */
    return OGS_OK;
}

int redis_poll_change_stream(void) {
    /* non-blocking redisGetReply loop. For each message:
     *   - on rich-channel: parse {"imsi","fields"} → ogs_dbi_change_event_t
     *     with high-fidelity updated_fields_mask. Record a short suppression
     *     window for that key.
     *   - on keyspace event: if key was just covered by a rich-channel
     *     event, drop. Otherwise emit with updated_fields_mask = ALL. */
}
```

### 5.3 Server configuration requirement

Redis must have keyspace events enabled. The recommended setting is at least
`Kg$` (key-space events + generic + string commands). The backend issues
`CONFIG SET notify-keyspace-events Kg$` at init; if denied, it logs a warning
and continues. **Documentation must call out** that users need
`notify-keyspace-events Kg$` in `redis.conf` for HSS change propagation to
work — including the example line for the docs.

### 5.4 Caveats

- Keyspace events are fire-and-forget. On hiredis disconnect, events between
  disconnect and reconnect are lost. The MongoDB change stream has the same
  property without resume tokens, so this is parity.
- The event payload only names the event (`set`, `del`); we re-fetch the
  JSON on each event rather than carrying the document in the notification.
- `PSUBSCRIBE` matching is O(N) in total subscribers. Not a Pi concern.

## 6. CLI tool — `open5gs-dbctl`

### 6.1 Purpose and scope

A small C command-line tool for provisioning the Redis backend. It is the
Pi alternative to the Node.js webui. The webui is **not** updated; users on
the Redis backend use the CLI.

### 6.2 Location and language

- Code: `tools/dbctl/` (new top-level).
- Binary: `open5gs-dbctl`, installed alongside the other open5gs binaries.
- Language: C. Reuses `lib/dbi/` and the Redis backend directly. No
  Python/Node toolchain on the Pi.
- Built only when `-Dredis=enabled` (or auto-detected).

### 6.3 Command surface

```
open5gs-dbctl --db-uri redis://localhost/ <subcommand>

  add        --imsi <imsi> --key <K> --opc <OPC> [--amf <amf>]
             [--apn <dnn> --sst <sst> [--sd <sd>] ...]
  del        --imsi <imsi>
  show       --imsi <imsi>                       # pretty-print JSON
  list       [--limit N]                         # SCAN + print IMSIs

  import     --file <path.json>                  # bulk import (mongoexport JSONL)
  export     --file <path.json>                  # bulk export (JSONL)

  msisdn     add --imsi <imsi> --msisdn <num>    # secondary index mgmt
  msisdn     del --msisdn <num>

  reset-sqn  --imsi <imsi> [--value 0]
```

### 6.4 Defaults for `add`

Match the webui's defaults so users don't have to remember every field:

- `subscriber_status` = 0 (service granted)
- `access_restriction_data` = 32 (handover to non-3GPP not allowed)
- `operator_determined_barring` = 0
- `network_access_mode` = 0
- Default slice: `sst` = 1, `default_indicator` = true, single session
  named "internet", type IPv4, 5QI = 9, ARP priority 8, pre-emption
  cap/vuln defaults

Power users use `import` with a hand-edited JSON file for anything else,
including IMS / iFC provisioning, which is too complex for flag-based
provisioning.

### 6.5 MSISDN index handling

`add` / `del` / `import` keep `open5gs:msisdn:<num>` → IMSI keys in sync.
The CLI is the only writer of these keys.

### 6.5.1 Change-event publishing

After any write that mutates an existing subscriber (`add` with an
existing IMSI = update, `del`, `import` that overwrites), the CLI
`PUBLISH`es a rich change event to `open5gs:events:subscriber` so the
HSS watcher can build a precise `updated_fields_mask`:

```
PUBLISH open5gs:events:subscriber {
  "imsi": "001010000000001",
  "fields": ["ambr", "slice"]
}
```

For pure inserts (new IMSI), publishing is optional but harmless.

### 6.6 Migration recipe (documented, not automated)

```sh
# On the old MongoDB host:
mongoexport --db=open5gs --collection=subscribers --jsonArray > subs.json

# On the new Redis host:
open5gs-dbctl --db-uri redis://localhost/ import --file subs.json
```

`import` must canonicalize MongoDB Extended JSON (`{"$numberLong": "12345"}`,
`{"$oid": "..."}`, etc.) into plain JSON the Redis-side reader expects.

### 6.7 Out of scope for v1

- IMS / iFC provisioning via CLI flags. Use `import` with a JSON file.
- Webui integration with Redis.
- Multi-tenancy / user accounts (CLI is single-user / root).

## 7. Build system and packaging

### 7.1 meson_options.txt

```meson
option('mongo', type: 'feature', value: 'auto',
       description: 'MongoDB backend for lib/dbi (libmongoc)')
option('redis', type: 'feature', value: 'auto',
       description: 'Redis backend for lib/dbi (hiredis)')
```

At configure time, error if both are disabled.

### 7.2 `lib/dbi/meson.build`

```meson
libdbi_sources = files(
    'ogs-dbi.h',
    'ogs-dbi-backend.h',
    'ogs-dbi-core.c',
    'subscription.c',
    'session.c',
    'ims.c',
)

backend_deps = []
backend_c_args = []

libmongoc_dep = dependency('libmongoc-1.0', required: get_option('mongo'))
if libmongoc_dep.found()
    libdbi_sources += files(
        'mongoc/mongoc-backend.c',
        'mongoc/mongoc-subscription.c',
        'mongoc/mongoc-session.c',
        'mongoc/mongoc-ims.c',
        'mongoc/mongoc-watch.c',
        'ogs-mongoc.c',
    )
    backend_deps += libmongoc_dep
    backend_c_args += '-DOGS_DBI_HAVE_MONGOC'
endif

libhiredis_dep = dependency('hiredis', required: get_option('redis'))
if libhiredis_dep.found()
    libdbi_sources += files(
        'redis/redis-backend.c',
        'redis/redis-subscription.c',
        'redis/redis-session.c',
        'redis/redis-ims.c',
        'redis/redis-watch.c',
    )
    backend_deps += libhiredis_dep
    backend_c_args += '-DOGS_DBI_HAVE_REDIS'
endif

if not libmongoc_dep.found() and not libhiredis_dep.found()
    error('At least one of -Dmongo / -Dredis must be enabled')
endif

libdbi = library('ogsdbi',
    sources : libdbi_sources,
    version : libogslib_version,
    c_args : ['-DOGS_DBI_COMPILATION'] + backend_c_args,
    include_directories : [libdbi_inc, libinc],
    dependencies : [libproto_dep] + backend_deps,
    install : true)

libdbi_dep = declare_dependency(
    link_with : libdbi,
    include_directories : [libdbi_inc, libinc],
    dependencies : [libproto_dep] + backend_deps)
```

### 7.3 Top-level `meson.build`

```meson
if libhiredis_dep.found()
    subdir('tools/dbctl')
endif
```

### 7.4 New dependency

`hiredis` (`libhiredis-dev` on Debian/Ubuntu/Raspbian, `hiredis` on
Alpine/Arch/Brew). C99, ~50 KB, sync API, no transitive deps.

### 7.5 Packaging

- `debian/control`: `Build-Depends` adds `libhiredis-dev`. New runtime
  package `open5gs-dbctl` for the CLI binary.
- `docker/`: existing Dockerfiles unaffected — auto-mode picks both
  backends up when their dev packages are installed.
- Add a `docker/Dockerfile.slim` example that builds with
  `-Dmongo=disabled -Dredis=enabled`.

### 7.6 Configs and examples

- `configs/*.yaml.in`: unchanged. Default stays `mongodb://localhost/open5gs`.
- New example `configs/redis.yaml.in` with
  `db_uri: redis://localhost/?prefix=open5gs:`.

### 7.7 Documentation

- `docs/_docs/guide/02-building-open5gs-from-sources.md`: build-flag matrix
  for mongo/redis enable/disable.
- New `docs/_docs/guide/redis-backend.md`:
  - Motivation (Pi use case, memory comparison).
  - Required `redis.conf` setting (`notify-keyspace-events Kg$`).
  - Migration recipe (`mongoexport` → `open5gs-dbctl import`).
  - `open5gs-dbctl` command reference.
  - Caveats (no Redis Cluster, managed-Redis CONFIG-SET limitations,
    notification loss on disconnect).

## 8. Testing

### 8.1 New test directories

- `tests/dbi/` — backend unit and equivalence tests.
- `tests/dbctl/` — CLI tests.

### 8.2 Test harness: ephemeral containers

Tests spawn their own backend containers on random ports:

- `redis:7-alpine` for Redis tests.
- `mongo:7` for Mongo tests and the equivalence test.

Harness flow: `docker run --rm -d ...` → capture port → set
`OGS_TEST_REDIS_URI` / `OGS_TEST_MONGO_URI` in the test process environment
→ run test binary → `docker stop` on teardown.

If `docker` is not available, tests **skip rather than fail** so
contributors without Docker can still build.

### 8.3 Backend unit tests

- `tests/dbi/mongoc-test.c` — smoke-tests the vtable wiring against Mongo.
- `tests/dbi/redis-test.c`:
  - `init` rejects malformed URIs cleanly.
  - `auth_info` returns expected K/OPC for a pre-loaded subscriber.
  - `update_sqn` → `auth_info` round-trip.
  - Concurrent `increment_sqn` from 4 threads — assert final value is exact.
  - `subscription_data` parses slices/sessions/PCC rules from JSON blob.
  - `msisdn_data` resolves via the secondary index key.
  - `ims_data` parses iFC / trigger-points / SPT trees correctly.
  - `update_imeisv` and `update_mme` round-trips.

### 8.4 Backend-equivalence test (load-bearing)

`tests/dbi/equivalence-test.c`:

- Load the same canonical JSON subscriber into both Mongo (via mongoc API)
  and Redis (via redis-cli).
- Call every read function (`auth_info`, `subscription_data`, `session_data`,
  `msisdn_data`, `ims_data`) against both backends.
- Assert byte-for-byte identical output structs.

This test is what guarantees Redis is a drop-in.

### 8.5 Integration tests against both backends

The existing test suites that touch the DB (`tests/registration/`,
`tests/handover/`, etc.) are run twice in CI:

- Once with `db_uri: mongodb://...`
- Once with `db_uri: redis://...`

Driven by an `OGS_TEST_DB_URI` env var consumed by the YAML config
templating. Same test code; proves the NFs are backend-agnostic.

### 8.6 CLI tests

`tests/dbctl/`:

- `add` → `show` round-trip.
- `import` of a fixture `subs.json` → `list` shows expected IMSIs.
- `export` then `import` into a fresh prefix round-trips identically.
- `del` removes both primary and MSISDN-index keys.
- Each test uses a random `prefix` query-string value to isolate runs.

### 8.7 Change-notification test

`tests/dbi/watch-test.c`:

- Start watcher; write to a subscriber key from a second connection.
- Assert the watcher pumps an event for it within a timeout.
- Both backends. Mongo variant requires replica-set; skip-if-not-replset.
  Redis variant runs `CONFIG SET notify-keyspace-events Kg$` explicitly
  and skips if denied.

### 8.8 CI matrix

- `meson setup -Dmongo=enabled -Dredis=disabled` — parity check.
- `meson setup -Dmongo=disabled -Dredis=enabled` — confirm libmongoc-free
  Pi-style build links and tests pass.
- `meson setup -Dmongo=enabled -Dredis=enabled` — default; equivalence
  tests run.

### 8.9 Manual Pi verification (soft pre-release check)

- Build `meson setup build -Dmongo=disabled -Dredis=enabled` on a
  Pi 4 (or Pi 3 if available).
- Note the absence of libmongoc and the resulting binary footprint.
- Provision a subscriber via `open5gs-dbctl`, register a UE, observe
  attach and `ogs_dbi_update_sqn` write going through.
- `htop` snapshot showing Redis RSS as anchor for the "lightweight on Pi"
  claim in docs.

## 9. Phasing

| Phase | Description | Estimate |
|-------|-------------|----------|
| 1 | Backend abstraction, no behavior change. Vtable + Mongo refactor into `mongoc/*.c`. **Introduce `ogs_dbi_change_event_t` and change HSS to consume the neutral payload** (signature change in `hss_handle_change_event`, event-bus field rename). BSON-walking code moves from `hss-context.c` into `mongoc-watch.c`. Existing tests pass. | ~500 new LOC, ~900 moved/modified. 3–4 days. |
| 2 | Redis backend, read paths only. `redis/redis-*.c` for reads. Meson + hiredis. Equivalence test passes for reads. | ~1100 LOC. 3–4 days. |
| 3 | Redis writes + change notifications. WATCH/MULTI for SQN/IMEISV/MME. `redis-watch.c` with rich-channel + keyspace-fallback notification model. 4G attach end-to-end against Redis. | ~550 LOC. 3 days. |
| 4 | `open5gs-dbctl` CLI, including rich-channel PUBLISH on writes. `tools/dbctl/`. Pi user can attach a UE without writing JSON by hand. | ~800 LOC. 3 days. |
| 5 | Docs, packaging, CI matrix. Dockerized integration tests. | ~50 LOC, ~200 docs. 1–2 days. |
| **Total** | | **~2800 new C LOC, ~1200 modified, ~13–16 focused days.** |

Each phase is independently testable and mergeable.

## 10. Risks

1. **IMS data walker complexity.** `lib/dbi/ims.c` is 420 lines of
   nine-level BSON tree traversal. Re-implementing it on `ogs_json` needs
   careful test coverage. The equivalence test is the safety net. May blow
   Phase 2 budget by 1 day.
2. **WATCH/MULTI semantics under HSS hammering SQN.** May need a bounded
   retry loop with logging; first cut might log false-positive contention
   warnings. May blow Phase 3 by half a day.
3. **`mongoexport` JSON format compatibility.** MongoDB Extended JSON
   (`$numberLong`, `$oid`, etc.) must be canonicalized in `import`.
   Handleable but a real footgun if missed. May blow Phase 4 by half a day.
4. **HSS event-bus signature change** (`hss_handle_change_event`,
   `e->dbi.document` → `e->dbi.change_event`). Touches `hss-context.c`,
   `hss-context.h`, `hss-sm.c`, `hss-timer.c`. Each call site is small and
   mechanical, but a missed edit will fail at link time. Phase 1 must
   compile clean against the full HSS build before moving on.
5. **Field-mask drift.** `OGS_DBI_FIELD_*` bits must stay in sync with
   what HSS actually examines. Any new field HSS starts caring about
   (post-merge) needs a new bit. Mitigation: keep `OGS_DBI_FIELD_ALL` as
   the safe fallback; document the contract in a comment on the enum.

## 11. Open questions

None at design time. Items to revisit during implementation:

- Whether `rediss://` (TLS) is straightforward via
  `redisConnectWithOptions` in hiredis. If yes, include in Phase 2;
  otherwise defer.
- Exact retry count and backoff for the WATCH/MULTI loop (likely 5
  attempts, no backoff — single-Pi has no real contention).
