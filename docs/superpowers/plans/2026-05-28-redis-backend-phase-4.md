# Redis Backend — Phase 4 Implementation Plan (`open5gs-dbctl-redis` CLI)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A C command-line tool, `open5gs-dbctl-redis`, to provision the Redis backend (add/del/show/list/import/export/msisdn/reset-sqn). This is the Pi alternative to the MongoDB-only Node.js webui — it lets users add/edit subscribers and migrate from MongoDB without MongoDB. Writes publish rich change events so a running HSS picks up edits (Phase 3 watcher).

**Architecture:** A standalone binary in `src/dbctl/`, built only when `-Dredis=enabled`. It is self-contained: links `libogscore` (for `ogs_strdup`/`ogs_msprintf`/logging) + `hiredis`, and compiles the already-vendored `lib/dbi/redis/cJSON.c` directly (no `libdbi` dependency, so it stays lean and builds in a libmongoc-free environment). It speaks raw Redis (SET/GET/SCAN/DEL/PUBLISH) using the SAME key conventions the backend reads: `<prefix>subscriber:<imsi>`, `<prefix>msisdn:<bcd>` → imsi, and publishes `<prefix>events:subscriber`. The JSON document shape mirrors the Mongo document 1:1 (the same shape the Phase-2 readers parse).

**Tech Stack:** C, meson/ninja, hiredis, vendored cJSON, libogscore, ABTS (pure-fn unit tests), Docker (integration test).

**Prerequisites:** Phases 1–3 complete (`phase-3-complete`). hiredis + Docker available. Build dir `build-phase1`.

**Decisions carried in (from brainstorming/spec §6 + this session):**
- Name `open5gs-dbctl-redis` (distinct from the existing Mongo shell `misc/db/open5gs-dbctl`).
- Redis-only tool (does not provision MongoDB).
- Basic subscriber provisioning via `add` flags (security + one APN/slice + default QoS, webui defaults); complex configs (IMS/iFC, multi-PCC) via `import` of a JSON file. No IMS/iFC flags in v1.
- `import` is `mongoexport`-compatible: accepts a JSON array (`mongoexport --jsonArray`) OR JSONL (one doc per line), and canonicalizes MongoDB Extended JSON (`{"$numberLong":"…"}`, `{"$oid":…}`, `{"$numberInt":…}`, `{"$date":…}`) to plain JSON.
- The tool maintains the MSISDN secondary index (the only writer of `<prefix>msisdn:*`).

**Build dir:** reuse `build-phase1`; `meson setup build-phase1 --reconfigure` after meson edits; NEVER delete it. Commit prefix `dbctl:`.

**Key-convention drift note:** the subscriber/msisdn/events key formats are duplicated from the redis backend (`lib/dbi/redis/redis-internal.h` constants / `redis_subscriber_key`). Keep them in sync; a shared header is overkill for v1, but add a comment in the CLI referencing the backend.

---

## webui defaults `add` must mirror (from `webui/server/models/subscriber.js`)
- `schema_version`: 1
- `access_restriction_data`: 32
- `subscriber_status`: 0
- `operator_determined_barring`: 0
- `network_access_mode`: 0
- `subscribed_rau_tau_timer`: 12
- slice: `sst` (required, default to 1 if `--sst` omitted), `default_indicator`: true, one session `{ name=<apn, default "internet">, type=3 (IPv4v6), qos{index=9, arp{priority_level=8, pre_emption_capability=1, pre_emption_vulnerability=1}}, ambr{downlink/uplink {value=1,unit=3}} }`
- `security`: `{ k, opc|op, amf (default "8000"), rand (optional) }` from flags; `sqn` initial 0.

## Command surface
```
open5gs-dbctl-redis --db-uri redis://host:port/?prefix=open5gs: <cmd> [args]

  add   --imsi <imsi> --key <K> (--opc <OPC> | --op <OP>) [--amf <amf>]
        [--apn <dnn>] [--sst <sst>] [--sd <sd>] [--msisdn <bcd>]
  del   --imsi <imsi>
  show  --imsi <imsi>
  list  [--limit <n>]
  import --file <path>            # mongoexport JSON array or JSONL
  export --file <path>            # JSONL
  msisdn-add --imsi <imsi> --msisdn <bcd>
  msisdn-del --msisdn <bcd>
  reset-sqn --imsi <imsi> [--value <n>]
```
(`--db-uri` may also be taken from env `DB_URI` if the flag is omitted.)

---

## File Map

**Created:**
- `src/dbctl/meson.build` — the `open5gs-dbctl-redis` executable, gated on `libhiredis_dep.found()`.
- `src/dbctl/dbctl.c` — `main`, argv/getopt_long parsing, command dispatch, output.
- `src/dbctl/dbctl-subscriber.h` / `dbctl-subscriber.c` — PURE functions: `dbctl_build_subscriber(const dbctl_add_args_t*) → cJSON*`; `dbctl_canonicalize_extended_json(cJSON *)` (in-place unwrap of Extended JSON); `dbctl_subscriber_imsi(cJSON*)`. Unit-testable, no Redis.
- `src/dbctl/dbctl-redis.h` / `dbctl-redis.c` — hiredis connection from a `redis://` URI; key builders; ops `dbctl_redis_set_subscriber`/`get`/`del`/`scan_imsis`/`set_msisdn_index`/`del_msisdn_index`/`publish_change`.
- `tests/dbctl/dbctl-test.c`, `tests/dbctl/meson.build` — ABTS unit tests for the pure functions (build + canonicalize).
- `tests/dbctl/fixtures/mongoexport.json` — a small `mongoexport --jsonArray` fixture (with `$numberLong` sqn) for import tests.

**Modified:**
- `src/meson.build` — `if libhiredis_dep.found(): subdir('dbctl')`.
- `tests/meson.build` — add `subdir('dbctl')` (unconditional — but the suite skips Redis-needing cases without `OGS_TEST_REDIS_URI`; pure-fn tests always run). NOTE: tests/dbctl compiles `src/dbctl/dbctl-subscriber.c` + the vendored cJSON, NOT gated on mongo, so it stays in the redis-only build too. Gate it on `libhiredis_dep.found()` to match the tool.
- `tests/dbi/run-redis-tests.sh` — optionally also run dbctl integration (or a new `tests/dbctl/run.sh`).
- `docs/_docs/...` / a migration note (light) — the `mongoexport → open5gs-dbctl-redis import` recipe.

---

## Task 1: CLI skeleton — meson target, URI connect, dispatch, `--help`

**Files:** create `src/dbctl/{meson.build,dbctl.c,dbctl-redis.h,dbctl-redis.c}`; modify `src/meson.build`.

- [ ] **Step 1: `src/dbctl/dbctl-redis.h`** (full AGPL header) — declare a `dbctl_redis_t` (host/port/prefix/ctx), `dbctl_redis_open(const char *uri, dbctl_redis_t *out)` / `dbctl_redis_close`, and the op prototypes (implemented across Tasks 2-4). Include `<hiredis.h>` and the vendored cJSON via a relative path the meson include dir resolves.

- [ ] **Step 2: `src/dbctl/dbctl-redis.c`** — implement `dbctl_redis_open` (parse `redis://host[:port]/?prefix=`, default port 6379, default prefix `open5gs:`, reject non-redis; `redisConnect`; on error return OGS_ERROR with a message) and `dbctl_redis_close` (`redisFree`). Provide `dbctl_redis_key(self, "subscriber", id)` → `ogs_msprintf("%ssubscriber:%s", prefix, id)` helper. (This duplicates the backend's URI/key logic; comment that it mirrors `lib/dbi/redis/`.)

- [ ] **Step 3: `src/dbctl/dbctl.c`** — `main`: getopt_long for `--db-uri`/`DB_URI`, subcommand as argv[1], `--help`/usage. For Task 1, dispatch table with stubs that print "not implemented" for every command except a working connectivity check: a hidden `ping` that opens the connection and `redisCommand(ctx, "PING")`. Exit non-zero on usage error / connect failure.

- [ ] **Step 4: `src/dbctl/meson.build`** — build `open5gs-dbctl-redis` from `dbctl.c`, `dbctl-redis.c`, `dbctl-subscriber.c` (created Task 2; for Task 1 omit it or create an empty stub), and the vendored `files('../../lib/dbi/redis/cJSON.c')`; `include_directories` for `src/dbctl` and `lib/dbi/redis`; `dependencies: [libcore_dep, libhiredis_dep]`; `install: true`. (`libcore_dep` and `libhiredis_dep` are in scope in src/.)

- [ ] **Step 5: wire into `src/meson.build`** — add at the end:
```meson
if libhiredis_dep.found()
    subdir('dbctl')
endif
```

- [ ] **Step 6: Build + smoke.** `meson setup build-phase1 --reconfigure && ninja -C build-phase1 src/dbctl/open5gs-dbctl-redis`. Run `./build-phase1/src/dbctl/open5gs-dbctl-redis --help` (prints usage, exit 0) and `... --db-uri redis://127.0.0.1:1 ping` (connect fails cleanly, non-zero, no crash).

- [ ] **Step 7: Commit** `dbctl: skeleton for open5gs-dbctl-redis (connect, dispatch, --help)`.

---

## Task 2: `add` (subscriber JSON builder) + `show` + `list` (TDD on the builder)

**Files:** create `src/dbctl/dbctl-subscriber.{h,c}`; modify `dbctl.c`, `dbctl-redis.c`; create `tests/dbctl/{dbctl-test.c,meson.build}`; modify `tests/meson.build`.

- [ ] **Step 1: define `dbctl_add_args_t`** in `dbctl-subscriber.h` (imsi, k, opc, op, use_opc, amf, rand, apn, sst, sd, has_sd, msisdn…), and prototype `cJSON *dbctl_build_subscriber(const dbctl_add_args_t *a);` and `const char *dbctl_subscriber_imsi(const cJSON *doc);`.

- [ ] **Step 2: failing unit test** in `tests/dbctl/dbctl-test.c`: build args {imsi 001010000000001, k=…, opc=…, apn "internet", sst 1}, call `dbctl_build_subscriber`, assert the resulting cJSON has `imsi`, `security.k`, `security.opc`, `access_restriction_data==32`, `subscriber_status==0`, `slice[0].sst==1`, `slice[0].session[0].name=="internet"`, `slice[0].session[0].qos.index==9`. Wire `tests/dbctl/meson.build` (compile `src/dbctl/dbctl-subscriber.c` + vendored cJSON + libcore_dep; ABTS via libcore) and `subdir('dbctl')` (gated `if libhiredis_dep.found()`) in tests/meson.build. Run → RED (link/undefined).

- [ ] **Step 3: implement `dbctl_build_subscriber`** — construct the cJSON document with the webui defaults (see the defaults section). Mirror the field names/nesting the Phase-2 reader expects (use the `OGS_*_STRING` values as literal keys, or hardcode the strings — they must match what `redis_parse_*` reads). `security`: k always; opc XOR op; amf default "8000"; sqn 0. One slice/session with QoS defaults. Add `msisdn` array if provided.

- [ ] **Step 4: implement `add`/`show`/`list` in dbctl.c + the redis ops** (`dbctl_redis_set_subscriber`, `get`, `scan_imsis`):
  - `add`: parse flags → args → `dbctl_build_subscriber` → `cJSON_PrintUnformatted` → `SET <prefix>subscriber:<imsi>`; if `--msisdn`, also `SET <prefix>msisdn:<bcd> <imsi>`. (Rich-event PUBLISH added in Task 3.)
  - `show`: `GET <prefix>subscriber:<imsi>` → `cJSON_Parse` → `cJSON_Print` (pretty) to stdout; error if missing.
  - `list`: `SCAN 0 MATCH <prefix>subscriber:* COUNT 100` loop → strip the key prefix → print each imsi; `--limit` caps output.
  - `dbctl_redis_set_subscriber` frees the serialized string with `cJSON_free`.

- [ ] **Step 5: build → GREEN** (unit test passes). Then a quick manual check against a throwaway redis container is optional (covered live in Task 5).

- [ ] **Step 6: Commit** `dbctl: add/show/list with subscriber JSON builder and unit tests`.

---

## Task 3: `del` + `msisdn-add`/`msisdn-del` + `reset-sqn` + rich-event publish

**Files:** `dbctl.c`, `dbctl-redis.c`, `dbctl-subscriber.c` (reuse).

- [ ] **Step 1: implement `dbctl_redis_publish_change(self, imsi, fields_json_array_or_NULL)`** — `PUBLISH <prefix>events:subscriber {"imsi":"<imsi>","fields":[...]}` (or `{"imsi":...}` with no fields → the watcher treats as ALL). Build the payload with cJSON.

- [ ] **Step 2: implement the commands:**
  - `del --imsi`: look up the subscriber to find its msisdn(s); `DEL <prefix>subscriber:<imsi>`; `DEL <prefix>msisdn:<each-msisdn>`; publish a change event for the imsi.
  - `msisdn-add --imsi --msisdn`: `SET <prefix>msisdn:<bcd> <imsi>` AND add the msisdn into the subscriber doc's `msisdn[]` (GET, mutate, SET) so reads stay consistent; publish change (fields `["msisdn"]`).
  - `msisdn-del --msisdn`: resolve imsi from the index, `DEL <prefix>msisdn:<bcd>`, remove from the subscriber doc's `msisdn[]`; publish.
  - `reset-sqn --imsi [--value n]`: GET → set `security.sqn = value` (default 0) → SET; publish (fields `[]`/none — SQN is not S6a-relevant, so publishing is optional; to avoid noise, do NOT publish for reset-sqn).
  - `add` (Task 2) and `import` (Task 4): after a write to an EXISTING imsi (overwrite), publish a change event; pure inserts may publish too (harmless).

- [ ] **Step 3: build + (the existing dbctl unit tests still pass).** Live publish behavior is verified in Task 5.

- [ ] **Step 4: Commit** `dbctl: del/msisdn/reset-sqn and rich change-event publishing`.

---

## Task 4: `import` (mongoexport Extended JSON) + `export` (TDD on the canonicalizer)

**Files:** `dbctl-subscriber.{h,c}` (canonicalizer), `dbctl.c`, `dbctl-redis.c`; `tests/dbctl/dbctl-test.c`, `tests/dbctl/fixtures/mongoexport.json`.

- [ ] **Step 1: failing unit test** for `dbctl_canonicalize_extended_json(cJSON *node)` (recursive, in-place): given `{"sqn":{"$numberLong":"96"},"_id":{"$oid":"abc"},"n":{"$numberInt":"5"}}`, after canonicalize: `sqn`==96 (number), `n`==5 (number), and `_id` is unwrapped to the string "abc" (or removed — decide: unwrap `$oid` to its string; the reader ignores `_id`). Assert via cJSON reads.

- [ ] **Step 2: implement `dbctl_canonicalize_extended_json`** — recursively walk objects/arrays; when an object has exactly one key that is an Extended-JSON wrapper, replace the node's value with the unwrapped primitive:
  - `$numberLong` / `$numberInt` (string or number) → JSON number.
  - `$oid` → string.
  - `$date` → number (epoch ms) or string (leave as-is if non-numeric) — best-effort; the reader ignores it.
  - Otherwise recurse into children. (cJSON in-place replacement: build the replacement item and use `cJSON_ReplaceItemInObjectCaseSensitive` / for array elements `cJSON_ReplaceItemInArray`.)

- [ ] **Step 3: implement `import`/`export` in dbctl.c:**
  - `import --file`: read the whole file; if it starts with `[` parse as a cJSON array; else split into lines and parse each as a doc (JSONL). For each doc: `dbctl_canonicalize_extended_json`; extract imsi via `dbctl_subscriber_imsi`; `SET <prefix>subscriber:<imsi>`; for each entry in the doc's `msisdn[]`, `SET <prefix>msisdn:<bcd> <imsi>`; publish change. Print a count.
  - `export --file`: `SCAN` all subscriber keys; for each, `GET` and write the JSON as one line (JSONL) to the file. Print a count.

- [ ] **Step 4: create `tests/dbctl/fixtures/mongoexport.json`** — a 1–2 subscriber `mongoexport --jsonArray` sample, including a `security.sqn` as `{"$numberLong":"96"}` and an `_id` `$oid`, matching the real mongoexport shape. Add an import round-trip unit/integration assertion where feasible (full import is Redis-backed → Task 5).

- [ ] **Step 5: build → GREEN** (canonicalizer unit test passes).

- [ ] **Step 6: Commit** `dbctl: import (mongoexport Extended JSON) and export`.

---

## Task 5: Docker integration test + migration recipe doc

**Files:** `tests/dbctl/run.sh` (or extend `tests/dbi/run-redis-tests.sh`), `tests/dbctl/dbctl-test.c` (integration cases gated on `OGS_TEST_REDIS_URI`), a docs note.

- [ ] **Step 1: integration test (Docker, runs live):** a script that starts `redis:7-alpine`, then:
  - `open5gs-dbctl-redis --db-uri redis://127.0.0.1:<port>/?prefix=test: add --imsi 001010000000001 --key <K> --opc <OPC> --apn internet --sst 1 --msisdn 491725670000`
  - Assert via a small C test (or `redis-cli`): `GET test:subscriber:001010000000001` parses, has the right k/opc/apn; `GET test:msisdn:491725670000` == the imsi.
  - **Cross-check with the backend reader:** build a tiny test that `ogs_dbi_init("redis://…/?prefix=test:")` then `ogs_dbi_auth_info("imsi-001010000000001",&a)` and `ogs_dbi_subscription_data(...)` succeed and match what `add` wrote — proving the CLI writes what the NF reads. (Reuse the equivalence-test harness pattern.)
  - `open5gs-dbctl-redis ... import --file tests/dbctl/fixtures/mongoexport.json`; `... list` shows the imported imsis; `... show --imsi <one>` prints it with the `$numberLong` sqn canonicalized to a number.
  - `... del --imsi 001010000000001`; assert the key and its msisdn index are gone.
  - Skips cleanly if `docker` absent.

- [ ] **Step 2: run it live** (Docker available) and capture output proving add→read-back, import→list/show, del→gone.

- [ ] **Step 3: migration recipe doc** — add a short doc/section: `mongoexport --db=open5gs --collection=subscribers --jsonArray > subs.json` then `open5gs-dbctl-redis --db-uri redis://localhost/ import --file subs.json`.

- [ ] **Step 4: Commit** `dbctl: live integration test + MongoDB→Redis migration recipe`.

---

## Task 6: Build matrix, redis-only, packaging, tag

- [ ] **Step 1: Build matrix.** Default `-Dmongo=enabled -Dredis=enabled` builds the tool + tests pass. `-Dmongo=disabled -Dredis=enabled` FULL build still succeeds and BUILDS `open5gs-dbctl-redis` (it must, since the CLI is the redis provisioning path); confirm it does not link libmongoc (`ldd … open5gs-dbctl-redis | grep -i mongoc` empty). `-Dmongo=disabled -Dredis=disabled` still errors at configure.
- [ ] **Step 2: packaging.** `debian/` — add `open5gs-dbctl-redis` to the appropriate package's installed binaries (or a new `open5gs-dbctl-redis` package). (Light; follow the existing debian packaging pattern. If out of scope for this session, note it for Phase 5.)
- [ ] **Step 3: Commit** any matrix/packaging fixes; tag `phase-4-complete` locally (do not push without user go-ahead).

---

## Self-Review

**Spec coverage (spec §6):** location/binary (Task 1, renamed `open5gs-dbctl-redis` per session decision); add/show/list (Task 2); del/msisdn/reset-sqn + rich publish (Task 3); import (mongoexport Extended JSON) + export (Task 4); migration recipe (Task 5); webui defaults (Task 2). IMS/iFC via import not flags = honored (no IMS flags). Redis-only = honored.

**Placeholder scan:** the pure functions (`dbctl_build_subscriber`, `dbctl_canonicalize_extended_json`) are the testable core and get unit tests; the Redis ops + dispatch are thin and covered by the Task-5 live integration test. Complete code is specified for the connection/URI, key building, the JSON shape (mirrors Phase-2 reader), and the canonicalizer algorithm.

**Type/name consistency:** `open5gs-dbctl-redis`; `dbctl_redis_t`/`dbctl_redis_open`/`_close`/`_key`/`_set_subscriber`/`_get`/`_del`/`_scan_imsis`/`_set_msisdn_index`/`_del_msisdn_index`/`_publish_change`; `dbctl_add_args_t`/`dbctl_build_subscriber`/`dbctl_canonicalize_extended_json`/`dbctl_subscriber_imsi`. Keys: `<prefix>subscriber:<imsi>`, `<prefix>msisdn:<bcd>`, `<prefix>events:subscriber` — must match `lib/dbi/redis/redis-internal.h`.

**Risks:**
1. **Key/shape drift** between the CLI and the backend reader — the integration test (Task 5) cross-checks by reading CLI-written data through `ogs_dbi_*`, which catches drift. Highest-value test.
2. **Extended JSON variety** — mongoexport emits several wrappers; the canonicalizer handles the common ones ($numberLong/$numberInt/$oid/$date) and recurses; unusual wrappers pass through (the reader ignores unknown fields like `_id`). 
3. **`cJSON_PrintUnformatted` allocator** — free with `cJSON_free` (the vendored copy provides it), consistent with the backend.
4. **getopt_long portability** — standard on Linux/glibc and the Pi; fine.
5. **Binary in `src/dbctl/` vs `misc/`** — placed in `src/` to match where open5gs binaries are built; gated on redis so non-redis builds don't build it.
