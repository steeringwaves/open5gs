# Redis Backend — Phase 5 Implementation Plan (docs / packaging / CI)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax. NOTE: this phase is docs/packaging/CI (markdown, YAML, debian, Dockerfile) — lower code-risk than Phases 1–4. Per-task two-stage *code* review is not needed; verify by building/inspecting the artifacts. Some artifacts (GitHub Actions, full debian build) only run in CI/packaging environments — validate the underlying commands locally and inspect for correctness.

**Goal:** Finish the Redis backend feature: a complete user-facing guide, the build-flag matrix in the build docs, debian packaging for `open5gs-dbctl-redis` (+ the libhiredis-dev build-dep), a CI build matrix (mongo / redis / both), and a redis-only (libmongoc-free) Docker example. After this, the feature is fully documented, packaged, and CI-guarded.

**Prerequisites:** Phases 1–4 complete (`phase-4-complete`). Build dir `build-phase1`. Docker available.

**Current state to build on:**
- `docs/_docs/guide/redis-backend.md` exists but is minimal (~39 lines, the Phase-4 migration note). Expand it.
- `docs/_docs/guide/02-building-open5gs-from-sources.md` exists — add the `-Dmongo`/`-Dredis` matrix.
- `debian/control` already has `libhiredis-dev` in Build-Depends (well-formed). `.install` files use `usr/bin/<binary>` lines. No entry for `open5gs-dbctl-redis` yet.
- `.github/workflows/meson-ci.yml` runs ONE Ubuntu job: installs libmongoc-dev+libhiredis-dev+libbson-dev, `meson setup build` (default = mongo+redis), `ninja`, `meson test -v` (with a mongodb service). Add matrix cells.
- `docker/{debian,alpine,ubuntu,fedora}/latest/{base,dev}/Dockerfile` exist. Add a redis-only slim example.

**Decisions:**
- Install `open5gs-dbctl-redis` via `debian/open5gs-common.install` (always-present common package; simplest, no new package stanza). It is built whenever hiredis-dev is available (the debian build has it as a Build-Dep), so the install path is reliably produced.
- CI matrix: add two cells to the existing workflow — a redis-only build (`-Dmongo=disabled -Dredis=enabled`, no mongodb service, runs the dbi/dbctl + core/unit suites) and a mongo-only build (`-Dmongo=enabled -Dredis=disabled`) — keeping the existing default (both) cell as the full integration run.

**Build dir:** reuse `build-phase1`. Commit prefix `docs:` / `debian:` / `ci:` as appropriate. Never delete the build dir.

---

## Task 1: Full Redis backend guide (`docs/_docs/guide/redis-backend.md`)

**Files:** `docs/_docs/guide/redis-backend.md` (expand).

- [ ] **Step 1:** Expand the doc to a complete guide (keep the existing migration recipe + Jekyll front-matter if present; check the top of the file and other guide docs for the front-matter pattern, e.g. `---\ntitle: ...\n---`). Sections:
  - **Why Redis** — MongoDB's `mongod` is ~100–200 MB resident; Redis is ~3 MB. Ideal for Raspberry Pi / small single-node 4G/5G cores.
  - **Building with the Redis backend** — `meson setup build -Dredis=enabled` (default auto-detects hiredis); for a libmongoc-free build, `meson setup build -Dmongo=disabled -Dredis=enabled`. Dependency: `libhiredis-dev`.
  - **Configuring** — set `db_uri: redis://localhost/` (optionally `redis://host:6379/?prefix=open5gs:`). One-line note that `rediss://` (TLS) is not yet supported.
  - **Provisioning subscribers** — `open5gs-dbctl-redis` command reference (add/show/list/del/msisdn-add/msisdn-del/reset-sqn/import/export) with one example each.
  - **Migrating from MongoDB** — the `mongoexport --jsonArray | open5gs-dbctl-redis import` recipe (already present — keep).
  - **Live subscriber updates (HSS)** — set `use_mongodb_change_stream: true` (works for both backends) and ensure Redis has `notify-keyspace-events Kg$` (the HSS best-effort sets it; otherwise put it in `redis.conf`). Cross-ref Phase 3.
  - **Caveats / limitations** — single-node only (no Redis Cluster/Sentinel); managed Redis that blocks `CONFIG SET` needs the redis.conf setting; change notifications are best-effort (lost across a watcher disconnect; a Redis restart needs an HSS restart to re-subscribe); the webui remains MongoDB-only (use the CLI for Redis).
- [ ] **Step 2:** Verify it renders as valid Markdown (no broken tables/code fences). If the site uses Jekyll `_docs` with a nav/order key, match neighboring docs' front-matter.
- [ ] **Step 3:** Commit `docs: complete the Redis backend guide`.

---

## Task 2: Build-flag matrix in the build-from-sources doc

**Files:** `docs/_docs/guide/02-building-open5gs-from-sources.md`.

- [ ] **Step 1:** Add a short "Database backend options" subsection documenting:
  - `-Dmongo=enabled|disabled|auto` (default auto; MongoDB via libmongoc).
  - `-Dredis=enabled|disabled|auto` (default auto; Redis via hiredis).
  - At least one must be enabled (configure errors otherwise).
  - Example: a Raspberry-Pi/redis-only build `meson setup build -Dmongo=disabled -Dredis=enabled` (no libmongoc/libbson needed).
  - Note the `libhiredis-dev` dependency next to the existing `libmongoc-dev` mention.
- [ ] **Step 2:** Commit `docs: document -Dmongo/-Dredis build options`.

---

## Task 3: debian packaging — install `open5gs-dbctl-redis`

**Files:** `debian/open5gs-common.install`, verify `debian/control`, `debian/changelog`.

- [ ] **Step 1:** Add `usr/bin/open5gs-dbctl-redis` to `debian/open5gs-common.install`. (The binary is built when hiredis-dev is present, which it is in the debian Build-Depends.)
- [ ] **Step 2:** Verify `debian/control` Build-Depends contains a well-formed `libhiredis-dev,` line (it does — confirm no stray backslash/missing comma from the earlier pre-commit hook). Fix if malformed.
- [ ] **Step 3:** Add a `debian/changelog` entry noting the Redis backend + `open5gs-dbctl-redis` (follow the existing changelog format/version pattern; do not invent a release — append an `UNRELEASED` or next-version stanza consistent with the repo's convention; if unsure, keep it minimal and note it).
- [ ] **Step 4 (verification, best-effort):** A full `dpkg-buildpackage` needs a packaging environment and is heavy; instead verify: (a) `debian/control` parses (e.g. `dpkg-checkbuilddeps` or a `python3 -c` parse, or just visual structure), (b) the `.install` glob path is correct (the binary installs to `usr/bin/open5gs-dbctl-redis` under the meson `bindir` — confirm by checking `build-phase1`'s install layout: `DESTDIR=/tmp/inst ninja -C build-phase1 install` then `ls /tmp/inst/usr/bin/open5gs-dbctl-redis`, then `rm -rf /tmp/inst`). This proves the install path the .install file references is real.
- [ ] **Step 5:** Commit `debian: package open5gs-dbctl-redis and confirm libhiredis-dev build-dep`.

---

## Task 4: CI build matrix (`.github/workflows/meson-ci.yml`)

**Files:** `.github/workflows/meson-ci.yml`.

- [ ] **Step 1:** Add two jobs (or a matrix) alongside the existing `ubuntu-latest` (default, both backends + mongodb service + full test):
  - **redis-only:** `ubuntu-latest`, install deps INCLUDING `libhiredis-dev` but the job does `meson setup build -Dmongo=disabled -Dredis=enabled` then `ninja -C build` then `meson test -C build -v --suite core --suite crypt --suite unit --suite dbi --suite dbctl` (the backend-agnostic + redis suites; NOT the mongo integration tests, which aren't built). No mongodb service. A `redis` service container (`image: redis`) so the dbi/dbctl Docker-less suites that need a server can use it — OR keep it to the non-server suites; the dbi/dbctl redis cases skip without `OGS_TEST_REDIS_URI`, so a redis service + setting that env makes them run. Prefer: add a `redis` service and export `OGS_TEST_REDIS_URI=redis://127.0.0.1:6379/?prefix=ci:` so the equivalence/watcher/dbctl integration cases execute in CI.
  - **mongo-only:** `meson setup build -Dmongo=enabled -Dredis=disabled` + `ninja` + the mongo integration tests (mongodb service), confirming the Mongo path still builds/works without redis.
- [ ] **Step 2 (local validation of the commands the workflow runs):** run each cell's meson/ninja locally in throwaway dirs (keep build-phase1): `-Dmongo=disabled -Dredis=enabled` build + `meson test --suite dbi --suite dbctl --suite unit` (with a local redis container + OGS_TEST_REDIS_URI for the live cases); `-Dmongo=enabled -Dredis=disabled` build. Confirm exit 0. (The YAML itself runs on GitHub; this validates the commands are correct.) Clean up throwaway dirs.
- [ ] **Step 3:** Commit `ci: add redis-only and mongo-only build matrix`.

---

## Task 5: Redis-only Docker example

**Files:** `docker/ubuntu/latest/dev/Dockerfile.redis` (or a documented variant), and a note in `docker/README.md`.

- [ ] **Step 1:** Add a minimal redis-only Dockerfile based on the existing ubuntu dev Dockerfile but: install `libhiredis-dev` (and NOT requiring libmongoc — though installing it is harmless, the point is the build flag), and `meson setup build -Dmongo=disabled -Dredis=enabled`. Keep it small; reuse the existing base image pattern. Name it clearly (e.g. `Dockerfile` under a new `docker/ubuntu/latest/redis/` or a `*.redis` suffix) and reference it in `docker/README.md`.
- [ ] **Step 2 (best-effort verification):** building the image is heavy; OPTIONAL — if time permits, `docker build` it to confirm the redis-only build succeeds in a clean container (this is the truest "builds without mongoc" proof). If skipped, note that the underlying `-Dmongo=disabled -Dredis=enabled` build is already verified locally (Phase 4 Task 6).
- [ ] **Step 3:** Commit `docker: add redis-only (libmongoc-free) build example`.

---

## Task 6: Final verification, whole-feature review, tag

- [ ] **Step 1: Full build matrix re-verify** (throwaway dirs, keep build-phase1): default mongo+redis (build + `meson test --suite dbi --suite dbctl --suite unit` pass); redis-only full build (exit 0, daemons + open5gs-dbctl-redis built, libmongoc-free via ldd); both-disabled configure error.
- [ ] **Step 2: Dispatch a final whole-feature holistic review** — review the entire `redis-main` branch vs `main` (or the spec) for coherence across all 5 phases: the vtable abstraction, the redis backend (reads/writes/watch), the CLI, docs/packaging/CI. Confirm no orphaned stubs, consistent naming, the spec's goals/non-goals all met or explicitly deferred. (This is the one place a review subagent adds value in Phase 5.)
- [ ] **Step 3:** Address any findings; commit.
- [ ] **Step 4:** Tag `phase-5-complete` (and optionally `redis-backend-complete`) locally. Do NOT push without user go-ahead.

---

## Self-Review

**Spec coverage (spec §7):** redis-backend.md guide (Task 1); build-flag matrix (Task 2); debian packaging incl. open5gs-dbctl-redis + libhiredis-dev (Task 3); CI matrix mongo/redis/both (Task 4); redis-only Docker example (Task 5); final verify + tag (Task 6). All spec §7 deliverables covered.

**Placeholder scan:** the only "best-effort/optional" steps are the full debian dpkg-buildpackage (Task 3 Step 4 — replaced by a real `DESTDIR install` path check) and the docker image build (Task 5 Step 2 — the underlying build flag is already verified). Everything else is concretely specified and locally verifiable.

**Risks:**
1. **Jekyll front-matter** — the docs site may require specific front-matter (title/order/nav). Match neighboring `_docs/guide/*.md` files so the new/expanded pages render in the nav.
2. **debian changelog convention** — don't fabricate a release version; append a stanza consistent with the repo (or note it for the maintainer). Keep minimal.
3. **CI redis service + OGS_TEST_REDIS_URI** — wiring the live redis cases in CI is the highest-value CI addition; ensure the service host/port and the env var match what the tests parse (`redis://127.0.0.1:6379/?prefix=ci:`).
4. **`.install` path** — verified by a real `DESTDIR` install (Task 3 Step 4), not assumed.
