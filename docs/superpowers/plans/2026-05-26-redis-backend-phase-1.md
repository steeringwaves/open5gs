# Redis Backend — Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refactor `lib/dbi/` to a vtable-based backend abstraction with no behavior change. Move all MongoDB-specific code into `lib/dbi/mongoc/`. Introduce a backend-neutral change-event payload (`ogs_dbi_change_event_t`) and migrate the HSS off direct BSON consumption. End state: identical functionality to today, but a second backend can be added in Phase 2 without touching `src/`.

**Architecture:** Public `ogs_dbi_*` functions become thin dispatchers that call `current_backend->vtable.fn(...)`. A backend registry holds entries keyed by URI scheme; `ogs_dbi_init(uri)` parses the scheme and picks the entry. Today only `"mongodb"` is registered. The mongoc backend lives in `lib/dbi/mongoc/*` and exposes only its registration symbol externally.

**Tech Stack:** C, meson build, ABTS test framework (existing in `tests/unit/`), libmongoc (existing).

**Scope reminder:** This plan covers **Phase 1 only** from the spec (`docs/superpowers/specs/2026-05-26-redis-backend-design.md`). Phases 2–5 (Redis backend reads, writes/watch, CLI, docs/packaging) get their own plans after Phase 1 lands and is validated.

**Branch convention:** Each task commits to the current branch (`redis-main`). Commit messages use `dbi:` prefix to group the work, matching the repo's existing style (see `git log --oneline`).

---

## File Map

**Created in Phase 1:**
- `lib/dbi/ogs-dbi-backend.h` — internal vtable + registry interface (not installed)
- `lib/dbi/ogs-dbi-core.c` — `ogs_dbi_init`/`final`, registry implementation, `ogs_dbi_change_event_*`
- `lib/dbi/mongoc/mongoc-internal.h` — internal declarations for the mongoc backend
- `lib/dbi/mongoc/mongoc-backend.c` — vtable registration + `ogs_mongoc_init`/`final` (moved from `ogs-mongoc.c`)
- `lib/dbi/mongoc/mongoc-subscription.c` — moved from `lib/dbi/subscription.c`
- `lib/dbi/mongoc/mongoc-session.c` — moved from `lib/dbi/session.c`
- `lib/dbi/mongoc/mongoc-ims.c` — moved from `lib/dbi/ims.c`
- `lib/dbi/mongoc/mongoc-watch.c` — change-stream loop + BSON→neutral-event translator (moved from `lib/dbi/ogs-mongoc.c` + `src/hss/hss-context.c`)
- `tests/dbi/abts-main.c` — ABTS runner for the new dbi test suite
- `tests/dbi/change-event-test.c` — unit tests for `ogs_dbi_change_event_*`
- `tests/dbi/backend-registry-test.c` — unit tests for vtable registry + scheme dispatch
- `tests/dbi/meson.build` — wires the new test executable

**Modified in Phase 1:**
- `lib/dbi/ogs-dbi.h` — add `ogs_dbi_field_t` enum, `ogs_dbi_change_event_t`, alloc/free declarations
- `lib/dbi/subscription.c` — rewritten as thin vtable dispatchers
- `lib/dbi/session.c` — rewritten as thin vtable dispatchers
- `lib/dbi/ims.c` — rewritten as thin vtable dispatchers
- `lib/dbi/ogs-mongoc.h` — body moved to `mongoc/mongoc-internal.h`; header becomes a thin compat shim that includes mongoc-internal.h when `OGS_DBI_HAVE_MONGOC` is set
- `lib/dbi/meson.build` — split source list, add `-Dmongo` feature gate
- `meson_options.txt` (top-level) — add `mongo` and `redis` feature options (`redis` placeholder for Phase 2)
- `src/hss/hss-event.h` — rename `dbi.document` to `dbi.change_event` (cosmetic; still `void *`)
- `src/hss/hss-context.h` — change `hss_handle_change_event` signature
- `src/hss/hss-context.c` — replace BSON-walking change-event handler with neutral-struct consumer; remove inline `process_change_stream` BSON loop (moves to `mongoc-watch.c`)
- `src/hss/hss-sm.c` — use `ogs_dbi_change_event_free` instead of `bson_destroy`
- `tests/meson.build` — add `subdir('dbi')`

**Deleted in Phase 1:**
- `lib/dbi/ogs-mongoc.c` — contents move to `mongoc/mongoc-backend.c` and `mongoc/mongoc-watch.c`

---

## Task 1: Add meson options for mongo / redis backends

**Files:**
- Modify: `meson_options.txt`

- [ ] **Step 1: Read the current options file**

Run: `cat meson_options.txt`
Expected output: shows `fuzzing` and `lib_fuzzing_engine` options.

- [ ] **Step 2: Append mongo and redis feature options**

Add to the end of `meson_options.txt`:

```meson
option('mongo', type: 'feature', value: 'auto',
       description: 'MongoDB backend for lib/dbi (libmongoc)')
option('redis', type: 'feature', value: 'auto',
       description: 'Redis backend for lib/dbi (hiredis). Phase 2; no-op in Phase 1.')
```

- [ ] **Step 3: Verify meson re-configures cleanly**

Run: `rm -rf build && meson setup build`
Expected: configure succeeds, includes `Mongo : YES` (or similar) in the summary output. No errors.

- [ ] **Step 4: Commit**

```bash
git add meson_options.txt
git commit -m "dbi: add -Dmongo / -Dredis meson feature options"
```

---

## Task 2: Add `ogs_dbi_change_event_t` to the public dbi header

**Files:**
- Modify: `lib/dbi/ogs-dbi.h`

This task adds the type **declaration only**. The implementation comes in Task 3.

- [ ] **Step 1: Read the current ogs-dbi.h**

Run: `cat lib/dbi/ogs-dbi.h`
Expected: see the current ~48-line header. Confirms there's no existing change-event type.

- [ ] **Step 2: Add the field enum and struct between the `#define OGS_DBI_INSIDE` block and the existing includes**

In `lib/dbi/ogs-dbi.h`, insert after `#define OGS_DBI_INSIDE` and before `#include "dbi/ogs-mongoc.h"`:

```c
#ifdef __cplusplus
extern "C" {
#endif

/*
 * Backend-neutral change-event payload, produced by the active backend's
 * change-notification source (Mongo change streams or Redis keyspace
 * events) and consumed by HSS. Each OGS_DBI_FIELD_* bit must stay in sync
 * with the fields HSS examines in hss_handle_change_event(); if HSS adds
 * a new field, add a bit here and update both backend producers.
 */
typedef enum {
    OGS_DBI_FIELD_REQUEST_CANCEL_LOCATION = 1u << 0,
    OGS_DBI_FIELD_ACCESS_RESTRICTION_DATA = 1u << 1,
    OGS_DBI_FIELD_SUBSCRIBER_STATUS       = 1u << 2,
    OGS_DBI_FIELD_OP_DETERMINED_BARRING   = 1u << 3,
    OGS_DBI_FIELD_NETWORK_ACCESS_MODE     = 1u << 4,
    OGS_DBI_FIELD_AMBR                    = 1u << 5,
    OGS_DBI_FIELD_RAU_TAU_TIMER           = 1u << 6,
    OGS_DBI_FIELD_SLICE                   = 1u << 7,
    OGS_DBI_FIELD_ALL                     = 0xFFFFFFFFu,
} ogs_dbi_field_e;

typedef struct ogs_dbi_change_event_s {
    char     *imsi_bcd;            /* heap-allocated via ogs_strdup */
    uint32_t  updated_fields_mask; /* bitmask of ogs_dbi_field_e */
} ogs_dbi_change_event_t;

ogs_dbi_change_event_t *ogs_dbi_change_event_alloc(
        const char *imsi_bcd, uint32_t updated_fields_mask);
void ogs_dbi_change_event_free(ogs_dbi_change_event_t *e);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 3: Verify the header is syntactically valid (compile a no-op TU)**

Run: `printf '#define OGS_DBI_COMPILATION\n#include "lib/dbi/ogs-dbi.h"\nint main(void) { return 0; }\n' > /tmp/check.c && gcc -c /tmp/check.c -Ilib -Ilib/core/include -Ilib/proto/include -Ilib/crypt/include -Ilib/dbi -o /tmp/check.o 2>&1 | head -5`

This is a smoke check; real validation comes when meson builds the lib in a later task.
Expected: it may complain about transitive headers (acceptable), but should NOT complain about the new types themselves.

- [ ] **Step 4: Commit (header-only change, no impl yet — that's fine)**

```bash
git add lib/dbi/ogs-dbi.h
git commit -m "dbi: declare ogs_dbi_change_event_t and field-mask enum"
```

---

## Task 3: Create `ogs-dbi-core.c` with `change_event_alloc/free` (TDD)

**Files:**
- Create: `lib/dbi/ogs-dbi-core.c`
- Create: `tests/dbi/abts-main.c`
- Create: `tests/dbi/change-event-test.c`
- Create: `tests/dbi/meson.build`
- Modify: `tests/meson.build`
- Modify: `lib/dbi/meson.build` (add ogs-dbi-core.c to source list)

- [ ] **Step 1: Write the failing test (`tests/dbi/change-event-test.c`)**

```c
/*
 * Copyright (C) 2026 by Sukchan Lee <acetcom@gmail.com>
 *
 * This file is part of Open5GS.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "ogs-dbi.h"
#include "core/abts.h"

static void change_event_alloc_populates_fields(abts_case *tc, void *data)
{
    ogs_dbi_change_event_t *e =
        ogs_dbi_change_event_alloc("001010000000001",
                OGS_DBI_FIELD_AMBR | OGS_DBI_FIELD_SLICE);

    ABTS_PTR_NOTNULL(tc, e);
    ABTS_STR_EQUAL(tc, "001010000000001", e->imsi_bcd);
    ABTS_INT_EQUAL(tc,
        OGS_DBI_FIELD_AMBR | OGS_DBI_FIELD_SLICE,
        (int) e->updated_fields_mask);

    ogs_dbi_change_event_free(e);
}

static void change_event_free_null_is_safe(abts_case *tc, void *data)
{
    ogs_dbi_change_event_free(NULL);
    ABTS_TRUE(tc, 1);   /* reached here without crashing */
}

static void change_event_alloc_null_imsi_returns_null(abts_case *tc, void *data)
{
    ogs_dbi_change_event_t *e =
        ogs_dbi_change_event_alloc(NULL, OGS_DBI_FIELD_ALL);
    ABTS_PTR_EQUAL(tc, NULL, e);
}

abts_suite *test_change_event(abts_suite *suite);

abts_suite *test_change_event(abts_suite *suite)
{
    suite = ADD_SUITE(suite);
    abts_run_test(suite, change_event_alloc_populates_fields, NULL);
    abts_run_test(suite, change_event_free_null_is_safe, NULL);
    abts_run_test(suite, change_event_alloc_null_imsi_returns_null, NULL);
    return suite;
}
```

- [ ] **Step 2: Write the ABTS runner (`tests/dbi/abts-main.c`)**

```c
/*
 * Copyright (C) 2026 by Sukchan Lee <acetcom@gmail.com>
 *
 * This file is part of Open5GS.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "ogs-dbi.h"
#include "core/abts.h"

abts_suite *test_change_event(abts_suite *suite);

const struct testlist {
    abts_suite *(*func)(abts_suite *suite);
} alltests[] = {
    { test_change_event },
    { NULL },
};

int main(int argc, const char *const *argv)
{
    int i;
    abts_suite *suite = NULL;

    ogs_core_initialize();
    atexit(ogs_core_terminate);

    for (i = 0; alltests[i].func; i++)
        suite = alltests[i].func(suite);

    return abts_report(suite);
}
```

- [ ] **Step 3: Write `tests/dbi/meson.build`**

```meson
# Copyright (C) 2026 by Sukchan Lee <acetcom@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

testdbi_sources = files('''
    abts-main.c
    change-event-test.c
'''.split())

testdbi_exe = executable('dbi-tests',
    sources : testdbi_sources,
    c_args : testunit_core_cc_flags,
    dependencies : [libdbi_dep, libcore_dep])

test('dbi', testdbi_exe, is_parallel : false, suite: 'dbi')
```

- [ ] **Step 4: Wire `tests/dbi/` into `tests/meson.build`**

In `tests/meson.build`, add after `subdir('unit')`:

```meson
subdir('dbi')
```

- [ ] **Step 5: Try to build — expect link failure (functions not yet defined)**

Run: `meson setup build --reconfigure && meson compile -C build dbi-tests`
Expected: link fails with `undefined reference to ogs_dbi_change_event_alloc` and `ogs_dbi_change_event_free`.

This is the "test fails" step — the failure is at link time, which is the appropriate "red" state for TDD on a C library API.

- [ ] **Step 6: Create the implementation file `lib/dbi/ogs-dbi-core.c`**

```c
/*
 * Copyright (C) 2026 by Sukchan Lee <acetcom@gmail.com>
 *
 * This file is part of Open5GS.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "ogs-dbi.h"

int __ogs_dbi_domain;

ogs_dbi_change_event_t *ogs_dbi_change_event_alloc(
        const char *imsi_bcd, uint32_t updated_fields_mask)
{
    ogs_dbi_change_event_t *e;

    if (!imsi_bcd)
        return NULL;

    e = ogs_calloc(1, sizeof(*e));
    ogs_assert(e);

    e->imsi_bcd = ogs_strdup(imsi_bcd);
    ogs_assert(e->imsi_bcd);
    e->updated_fields_mask = updated_fields_mask;

    return e;
}

void ogs_dbi_change_event_free(ogs_dbi_change_event_t *e)
{
    if (!e)
        return;
    if (e->imsi_bcd)
        ogs_free(e->imsi_bcd);
    ogs_free(e);
}
```

- [ ] **Step 7: Remove the duplicate `__ogs_dbi_domain` definition from `lib/dbi/ogs-mongoc.c`**

`__ogs_dbi_domain` is now defined in `ogs-dbi-core.c` (Step 6). The old file still defines it, which will fail at link time as a duplicate symbol.

Find the line `int __ogs_dbi_domain;` near the top of `lib/dbi/ogs-mongoc.c` and delete it. (The definition will be re-introduced briefly in Task 6 only inside the mongoc backend's namespace, but `ogs-mongoc.c` itself is deleted in Task 9.)

- [ ] **Step 8: Add `ogs-dbi-core.c` to `lib/dbi/meson.build` sources**

In `lib/dbi/meson.build`, modify the `libdbi_sources` list to include `ogs-dbi-core.c`:

```meson
libdbi_sources = files('''
    ogs-dbi.h

    ogs-mongoc.h

    ogs-dbi-core.c
    ogs-mongoc.c
    subscription.c
    session.c
    ims.c
'''.split())
```

- [ ] **Step 9: Build and run the tests — expect pass**

Run: `meson compile -C build && meson test -C build --suite dbi -v`
Expected: 3 tests pass under suite "dbi".

- [ ] **Step 10: Commit**

```bash
git add lib/dbi/ogs-dbi-core.c lib/dbi/ogs-mongoc.c lib/dbi/meson.build \
        tests/dbi/abts-main.c tests/dbi/change-event-test.c \
        tests/dbi/meson.build tests/meson.build
git commit -m "dbi: implement ogs_dbi_change_event_alloc/free with ABTS tests"
```

---

## Task 4: Add backend vtable type (`ogs-dbi-backend.h`)

**Files:**
- Create: `lib/dbi/ogs-dbi-backend.h`
- Modify: `lib/dbi/meson.build` (add header to source list)

This task only declares the vtable type and registry API; the registry implementation comes in Task 5.

- [ ] **Step 1: Create the header**

Create `lib/dbi/ogs-dbi-backend.h`:

```c
/*
 * Copyright (C) 2026 by Sukchan Lee <acetcom@gmail.com>
 *
 * This file is part of Open5GS.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * INTERNAL header. Only included by lib/dbi/*.c and lib/dbi/<backend>/*.c.
 * Not installed.
 */

#if !defined(OGS_DBI_INSIDE) && !defined(OGS_DBI_COMPILATION)
#error "This header cannot be included directly."
#endif

#ifndef OGS_DBI_BACKEND_H
#define OGS_DBI_BACKEND_H

#include "ogs-dbi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ogs_dbi_backend_s {
    const char *name;       /* "mongoc", "redis" */
    const char *scheme;     /* "mongodb", "redis" */

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

    /* Optional. NULL means "this backend does not support change events." */
    int  (*watch_init)(void);
    int  (*poll_change_stream)(void);
} ogs_dbi_backend_t;

/*
 * Backend registration. Called from each backend's translation unit at
 * library-load time (or explicitly from ogs_dbi_init's dispatch table).
 * Returns OGS_OK on success, OGS_ERROR if a backend with the same scheme
 * is already registered.
 */
int ogs_dbi_backend_register(const ogs_dbi_backend_t *backend);

/* Lookup by URI scheme ("mongodb", "redis"). Returns NULL if not found. */
const ogs_dbi_backend_t *ogs_dbi_backend_find(const char *scheme);

/* The currently active backend, set by ogs_dbi_init(). NULL before init. */
const ogs_dbi_backend_t *ogs_dbi_current_backend(void);

#ifdef __cplusplus
}
#endif

#endif /* OGS_DBI_BACKEND_H */
```

- [ ] **Step 2: Add the header to `libdbi_sources` in `lib/dbi/meson.build`**

```meson
libdbi_sources = files('''
    ogs-dbi.h
    ogs-dbi-backend.h

    ogs-mongoc.h

    ogs-dbi-core.c
    ogs-mongoc.c
    subscription.c
    session.c
    ims.c
'''.split())
```

- [ ] **Step 3: Build to confirm the header is syntactically valid**

Run: `meson compile -C build`
Expected: clean build (header is only declarations, no impl yet).

- [ ] **Step 4: Commit**

```bash
git add lib/dbi/ogs-dbi-backend.h lib/dbi/meson.build
git commit -m "dbi: declare backend vtable type and registry API"
```

---

## Task 5: Implement backend registry + scheme dispatch (TDD)

**Files:**
- Modify: `lib/dbi/ogs-dbi-core.c` (add registry, init, final)
- Create: `tests/dbi/backend-registry-test.c`
- Modify: `tests/dbi/abts-main.c` (register the new suite)
- Modify: `tests/dbi/meson.build` (add new test source)

- [ ] **Step 1: Write the failing test `tests/dbi/backend-registry-test.c`**

```c
/*
 * Copyright (C) 2026 by Sukchan Lee <acetcom@gmail.com>
 *
 * This file is part of Open5GS.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#define OGS_DBI_COMPILATION
#include "ogs-dbi.h"
#include "ogs-dbi-backend.h"
#include "core/abts.h"

static int stub_init_called = 0;
static int stub_final_called = 0;
static char stub_last_uri[256];

static int stub_init(const char *uri)
{
    stub_init_called++;
    if (uri) {
        ogs_cpystrn(stub_last_uri, uri, sizeof(stub_last_uri));
    } else {
        stub_last_uri[0] = '\0';
    }
    return OGS_OK;
}

static void stub_final(void)
{
    stub_final_called++;
}

static const ogs_dbi_backend_t stub_backend = {
    .name   = "stub",
    .scheme = "stub",
    .init   = stub_init,
    .final  = stub_final,
};

static void reset_stub(void)
{
    stub_init_called = 0;
    stub_final_called = 0;
    stub_last_uri[0] = '\0';
}

static void registry_register_then_find(abts_case *tc, void *data)
{
    int rv = ogs_dbi_backend_register(&stub_backend);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);

    const ogs_dbi_backend_t *found = ogs_dbi_backend_find("stub");
    ABTS_PTR_EQUAL(tc, (void *)&stub_backend, (void *)found);
}

static void registry_double_register_fails(abts_case *tc, void *data)
{
    /* stub_backend registered by previous test */
    int rv = ogs_dbi_backend_register(&stub_backend);
    ABTS_INT_EQUAL(tc, OGS_ERROR, rv);
}

static void registry_find_unknown_returns_null(abts_case *tc, void *data)
{
    ABTS_PTR_EQUAL(tc, NULL,
        (void *)ogs_dbi_backend_find("does-not-exist"));
}

static void init_dispatches_by_scheme(abts_case *tc, void *data)
{
    reset_stub();

    int rv = ogs_dbi_init("stub://host:1234/dbname");
    ABTS_INT_EQUAL(tc, OGS_OK, rv);
    ABTS_INT_EQUAL(tc, 1, stub_init_called);
    ABTS_STR_EQUAL(tc, "stub://host:1234/dbname", stub_last_uri);
    ABTS_PTR_EQUAL(tc, (void *)&stub_backend,
        (void *)ogs_dbi_current_backend());

    ogs_dbi_final();
    ABTS_INT_EQUAL(tc, 1, stub_final_called);
    ABTS_PTR_EQUAL(tc, NULL, (void *)ogs_dbi_current_backend());
}

static void init_unknown_scheme_returns_error(abts_case *tc, void *data)
{
    int rv = ogs_dbi_init("nosuch://host/db");
    ABTS_INT_EQUAL(tc, OGS_ERROR, rv);
}

static void init_null_uri_returns_error(abts_case *tc, void *data)
{
    int rv = ogs_dbi_init(NULL);
    ABTS_INT_EQUAL(tc, OGS_ERROR, rv);
}

abts_suite *test_backend_registry(abts_suite *suite);

abts_suite *test_backend_registry(abts_suite *suite)
{
    suite = ADD_SUITE(suite);
    abts_run_test(suite, registry_register_then_find, NULL);
    abts_run_test(suite, registry_double_register_fails, NULL);
    abts_run_test(suite, registry_find_unknown_returns_null, NULL);
    abts_run_test(suite, init_dispatches_by_scheme, NULL);
    abts_run_test(suite, init_unknown_scheme_returns_error, NULL);
    abts_run_test(suite, init_null_uri_returns_error, NULL);
    return suite;
}
```

- [ ] **Step 2: Add the suite to `tests/dbi/abts-main.c`**

Modify `tests/dbi/abts-main.c` to declare and add the new suite to `alltests[]`:

```c
abts_suite *test_change_event(abts_suite *suite);
abts_suite *test_backend_registry(abts_suite *suite);

const struct testlist {
    abts_suite *(*func)(abts_suite *suite);
} alltests[] = {
    { test_change_event },
    { test_backend_registry },
    { NULL },
};
```

- [ ] **Step 3: Add the new source to `tests/dbi/meson.build`**

```meson
testdbi_sources = files('''
    abts-main.c
    change-event-test.c
    backend-registry-test.c
'''.split())
```

- [ ] **Step 4: Build — expect link failure**

Run: `meson compile -C build dbi-tests`
Expected: undefined references to `ogs_dbi_backend_register`, `ogs_dbi_backend_find`, `ogs_dbi_init`, `ogs_dbi_final`, `ogs_dbi_current_backend`.

- [ ] **Step 5: Implement the registry and dispatch in `lib/dbi/ogs-dbi-core.c`**

Replace the contents of `lib/dbi/ogs-dbi-core.c` with:

```c
/*
 * Copyright (C) 2026 by Sukchan Lee <acetcom@gmail.com>
 *
 * This file is part of Open5GS.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#define OGS_DBI_COMPILATION
#include "ogs-dbi.h"
#include "ogs-dbi-backend.h"

int __ogs_dbi_domain;

#define OGS_DBI_MAX_BACKENDS 4

static struct {
    const ogs_dbi_backend_t *entries[OGS_DBI_MAX_BACKENDS];
    int count;
} backends;

static const ogs_dbi_backend_t *active;

int ogs_dbi_backend_register(const ogs_dbi_backend_t *backend)
{
    int i;

    ogs_assert(backend);
    ogs_assert(backend->scheme);

    for (i = 0; i < backends.count; i++) {
        if (!strcmp(backends.entries[i]->scheme, backend->scheme)) {
            ogs_warn("dbi backend [%s] already registered", backend->scheme);
            return OGS_ERROR;
        }
    }

    if (backends.count >= OGS_DBI_MAX_BACKENDS) {
        ogs_error("dbi backend table full");
        return OGS_ERROR;
    }

    backends.entries[backends.count++] = backend;
    return OGS_OK;
}

const ogs_dbi_backend_t *ogs_dbi_backend_find(const char *scheme)
{
    int i;

    if (!scheme)
        return NULL;

    for (i = 0; i < backends.count; i++) {
        if (!strcmp(backends.entries[i]->scheme, scheme))
            return backends.entries[i];
    }
    return NULL;
}

const ogs_dbi_backend_t *ogs_dbi_current_backend(void)
{
    return active;
}

/*
 * Extract the scheme prefix from a URI: "mongodb://..." -> "mongodb".
 * Returns a heap-allocated string the caller must ogs_free, or NULL on
 * malformed input.
 */
static char *extract_scheme(const char *uri)
{
    const char *sep;
    size_t len;
    char *out;

    if (!uri)
        return NULL;

    sep = strstr(uri, "://");
    if (!sep)
        return NULL;

    len = sep - uri;
    out = ogs_malloc(len + 1);
    ogs_assert(out);
    memcpy(out, uri, len);
    out[len] = '\0';
    return out;
}

int ogs_dbi_init(const char *uri)
{
    char *scheme;
    const ogs_dbi_backend_t *backend;
    int rv;

    if (!uri) {
        ogs_error("dbi init: NULL URI");
        return OGS_ERROR;
    }

    scheme = extract_scheme(uri);
    if (!scheme) {
        ogs_error("dbi init: malformed URI '%s' (missing scheme://)", uri);
        return OGS_ERROR;
    }

    backend = ogs_dbi_backend_find(scheme);
    if (!backend) {
        ogs_error("dbi init: no backend registered for scheme '%s'", scheme);
        ogs_free(scheme);
        return OGS_ERROR;
    }
    ogs_free(scheme);

    rv = backend->init(uri);
    if (rv != OGS_OK)
        return rv;

    active = backend;
    return OGS_OK;
}

void ogs_dbi_final(void)
{
    if (active && active->final)
        active->final();
    active = NULL;
}

/* Legacy collection-watch entry points. Phase 1 keeps the existing public
 * signatures and dispatches through the vtable; the per-backend
 * implementations move into mongoc/mongoc-watch.c and (Phase 3)
 * redis/redis-watch.c. */
int ogs_dbi_collection_watch_init(void)
{
    if (!active || !active->watch_init) {
        ogs_warn("dbi: active backend does not support change watching");
        return OGS_ERROR;
    }
    return active->watch_init();
}

int ogs_dbi_poll_change_stream(void)
{
    if (!active || !active->poll_change_stream)
        return OGS_ERROR;
    return active->poll_change_stream();
}

ogs_dbi_change_event_t *ogs_dbi_change_event_alloc(
        const char *imsi_bcd, uint32_t updated_fields_mask)
{
    ogs_dbi_change_event_t *e;

    if (!imsi_bcd)
        return NULL;

    e = ogs_calloc(1, sizeof(*e));
    ogs_assert(e);
    e->imsi_bcd = ogs_strdup(imsi_bcd);
    ogs_assert(e->imsi_bcd);
    e->updated_fields_mask = updated_fields_mask;

    return e;
}

void ogs_dbi_change_event_free(ogs_dbi_change_event_t *e)
{
    if (!e)
        return;
    if (e->imsi_bcd)
        ogs_free(e->imsi_bcd);
    ogs_free(e);
}
```

- [ ] **Step 6: Remove the existing `ogs_dbi_init`/`ogs_dbi_final`/`ogs_dbi_collection_watch_init`/`ogs_dbi_poll_change_stream` definitions from `lib/dbi/ogs-mongoc.c`**

These four functions are now provided by `ogs-dbi-core.c`. The mongoc-specific work they used to do (creating the `subscribers` collection handle, opening the change stream) moves into the mongoc backend's `init` / `watch_init` callbacks in Task 6.

Delete (from `lib/dbi/ogs-mongoc.c`):
- `int ogs_dbi_init(const char *db_uri)` and its body
- `void ogs_dbi_final(void)` and its body
- `int ogs_dbi_collection_watch_init(void)` and its body

The remaining mongoc-specific functions (`ogs_mongoc_init`, `ogs_mongoc_final`, `ogs_mongoc()`) stay in `ogs-mongoc.c` for now; they move in Task 6.

- [ ] **Step 7: Build the tests — expect them all to compile but registry tests still fail because no backend is registered for the previously-tested scheme "mongodb"**

Run: `meson compile -C build dbi-tests && meson test -C build --suite dbi -v`
Expected: All 9 dbi tests pass (the "stub" backend the tests register satisfies their needs; no dependence on mongo).

- [ ] **Step 8: Confirm the main open5gs build still links**

Run: `meson compile -C build`
Expected: build proceeds past the lib/dbi step. **Expect a link error in the HSS or NF executables** because their `ogs_dbi_init` callers no longer find the mongo init logic (no backend registered). That's fine; we will fix it in Task 6 by registering the mongoc backend.

If the link error is anything other than "no mongo backend registered" or related to missing mongoc state setup, stop and diagnose.

- [ ] **Step 9: Commit (acknowledge the temporary breakage in the commit message)**

```bash
git add lib/dbi/ogs-dbi-core.c lib/dbi/ogs-mongoc.c \
        tests/dbi/backend-registry-test.c tests/dbi/abts-main.c \
        tests/dbi/meson.build
git commit -m "dbi: add backend registry and scheme dispatch

The mongoc backend's init/final/watch_init bodies temporarily move into
this commit's tombstoned form; full NF link is restored in the next
commit when mongoc-backend.c registers itself."
```

---

## Task 6: Create `mongoc-backend.c` (vtable registration + init/final)

**Files:**
- Create: `lib/dbi/mongoc/mongoc-internal.h`
- Create: `lib/dbi/mongoc/mongoc-backend.c`
- Modify: `lib/dbi/ogs-mongoc.c` (delete `ogs_mongoc_init`/`final` bodies — they move)
- Modify: `lib/dbi/ogs-mongoc.h` (keep as a public compat shim — point to internal types)
- Modify: `lib/dbi/meson.build`

- [ ] **Step 1: Create `lib/dbi/mongoc/mongoc-internal.h`**

```c
/*
 * Copyright (C) 2026 by Sukchan Lee <acetcom@gmail.com>
 *
 * This file is part of Open5GS.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Internal mongoc-backend types and functions. Included only by
 * lib/dbi/mongoc/*.c, never by callers.
 */

#ifndef OGS_DBI_MONGOC_INTERNAL_H
#define OGS_DBI_MONGOC_INTERNAL_H

#define OGS_DBI_COMPILATION
#include "ogs-dbi.h"
#include "ogs-dbi-backend.h"

#include <mongoc.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ogs_mongoc_s {
    bool initialized;
    const char *name;
    void *uri;
    void *client;
    void *database;

#if MONGOC_CHECK_VERSION(1, 9, 0)
    mongoc_change_stream_t *stream;
#endif

    char *masked_db_uri;

    struct {
        void *subscriber;
    } collection;
} ogs_mongoc_t;

/* Implementation lives in mongoc-backend.c */
int  ogs_mongoc_init(const char *db_uri);
void ogs_mongoc_final(void);
ogs_mongoc_t *ogs_mongoc(void);

/* The vtable instance registered by mongoc_backend_register(). */
extern const ogs_dbi_backend_t mongoc_backend;

/* Backend method declarations — implemented across mongoc-{subscription,
 * session, ims, watch}.c. Naming convention: drop the `ogs_dbi_` prefix,
 * prepend `mongoc_`. */

int  mongoc_auth_info(char *supi, ogs_dbi_auth_info_t *out);
int  mongoc_update_sqn(char *supi, uint64_t sqn);
int  mongoc_increment_sqn(char *supi);
int  mongoc_update_imeisv(char *supi, char *imeisv);
int  mongoc_update_mme(char *supi, char *host, char *realm, bool purge);

int  mongoc_subscription_data(char *supi, ogs_subscription_data_t *out);
int  mongoc_session_data(const char *supi, const ogs_s_nssai_t *s_nssai,
                         const char *dnn, ogs_session_data_t *out);
int  mongoc_msisdn_data(char *id, ogs_msisdn_data_t *out);
int  mongoc_ims_data(char *supi, ogs_ims_data_t *out);

int  mongoc_watch_init(void);
int  mongoc_poll_change_stream(void);

#ifdef __cplusplus
}
#endif

#endif /* OGS_DBI_MONGOC_INTERNAL_H */
```

- [ ] **Step 2: Create `lib/dbi/mongoc/mongoc-backend.c`**

```c
/*
 * Copyright (C) 2026 by Sukchan Lee <acetcom@gmail.com>
 *
 * This file is part of Open5GS.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * MongoDB backend for lib/dbi. Originally split out of ogs-mongoc.c during
 * the Phase 1 refactor that introduced runtime-selectable backends.
 */

#include "mongoc-internal.h"

static ogs_mongoc_t self;

const ogs_dbi_backend_t mongoc_backend = {
    .name              = "mongoc",
    .scheme            = "mongodb",
    .init              = ogs_mongoc_init,
    .final             = ogs_mongoc_final,

    .auth_info         = mongoc_auth_info,
    .update_sqn        = mongoc_update_sqn,
    .increment_sqn     = mongoc_increment_sqn,
    .update_imeisv     = mongoc_update_imeisv,
    .update_mme        = mongoc_update_mme,

    .subscription_data = mongoc_subscription_data,
    .session_data      = mongoc_session_data,
    .msisdn_data       = mongoc_msisdn_data,
    .ims_data          = mongoc_ims_data,

    .watch_init        = mongoc_watch_init,
    .poll_change_stream = mongoc_poll_change_stream,
};

/* Constructor — runs before main() at library load. Registers the
 * backend with the core dispatcher so ogs_dbi_init("mongodb://...") finds
 * it without an explicit registration call from the application. */
__attribute__((constructor))
static void mongoc_backend_register(void)
{
    if (ogs_dbi_backend_register(&mongoc_backend) != OGS_OK) {
        /* This should not happen in a single-process load.
         * Log via fputs because the log domain may not be configured yet. */
        fputs("mongoc: failed to register backend\n", stderr);
    }
}

/* ------------------------------------------------------------------ */
/* The functions below are MOVED from lib/dbi/ogs-mongoc.c. Body is    */
/* identical except for two changes:                                    */
/*   1. Renamed from `ogs_dbi_init` / `ogs_dbi_final` to                */
/*      `ogs_mongoc_init` / `ogs_mongoc_final` — they are now the       */
/*      mongoc backend's init/final callbacks, not the public API.      */
/*      (Public `ogs_dbi_init` lives in ogs-dbi-core.c and dispatches  */
/*      through the vtable.)                                            */
/*   2. The two-step init in the old code (`ogs_mongoc_init` then       */
/*      `ogs_dbi_init` opening the subscriber collection) is collapsed  */
/*      into one function here.                                         */
/* ------------------------------------------------------------------ */

static bool
mongoc_get_server_status(mongoc_client_t *client,
                         mongoc_read_prefs_t *read_prefs,
                         bson_t *reply, bson_error_t *error)
{
    bson_t cmd = BSON_INITIALIZER;
    bool ret;

    BSON_ASSERT(client);
    BSON_APPEND_INT32(&cmd, "ping", 1);
    ret = mongoc_client_command_simple(
            client, "admin", &cmd, read_prefs, reply, error);
    bson_destroy(&cmd);
    return ret;
}

static char *masked_db_uri(const char *db_uri)
{
    char *tmp, *masked = NULL, *saveptr = NULL;
    char *array[2];

    ogs_assert(db_uri);
    tmp = ogs_strdup(db_uri);
    ogs_assert(tmp);

    memset(array, 0, sizeof(array));
    array[0] = ogs_strtok_r(tmp, "@", &saveptr);
    if (array[0])
        array[1] = ogs_strtok_r(NULL, "@", &saveptr);

    if (array[1]) {
        masked = ogs_msprintf("mongodb://*****:*****@%s", array[1]);
        ogs_assert(masked);
    } else {
        masked = ogs_strdup(array[0]);
        ogs_assert(masked);
    }
    ogs_free(tmp);
    return masked;
}

int ogs_mongoc_init(const char *db_uri)
{
    bson_t reply;
    bson_error_t error;
    bson_iter_t iter;
    const mongoc_uri_t *uri;

    if (!db_uri) {
        ogs_error("No DB_URI");
        return OGS_ERROR;
    }

    memset(&self, 0, sizeof(ogs_mongoc_t));

    self.masked_db_uri = masked_db_uri(db_uri);

    mongoc_init();
    self.initialized = true;

    self.client = mongoc_client_new(db_uri);
    if (!self.client) {
        ogs_error("Failed to parse DB URI [%s]", self.masked_db_uri);
        return OGS_ERROR;
    }

#if MONGOC_CHECK_VERSION(1, 4, 0)
    mongoc_client_set_error_api(self.client, 2);
#endif

    uri = mongoc_client_get_uri(self.client);
    ogs_assert(uri);

    self.name = mongoc_uri_get_database(uri);
    ogs_assert(self.name);

    self.database = mongoc_client_get_database(self.client, self.name);
    ogs_assert(self.database);

    if (!mongoc_get_server_status(self.client, NULL, &reply, &error)) {
        ogs_warn("Failed to connect to server [%s]", self.masked_db_uri);
        return OGS_RETRY;
    }
    ogs_assert(bson_iter_init_find(&iter, &reply, "ok"));
    bson_destroy(&reply);

    ogs_info("MongoDB URI: '%s'", self.masked_db_uri);

    /* Open the subscriber collection — was previously done in the old
     * ogs_dbi_init(). */
    self.collection.subscriber = mongoc_client_get_collection(
            self.client, self.name, "subscribers");
    ogs_assert(self.collection.subscriber);

    return OGS_OK;
}

void ogs_mongoc_final(void)
{
    if (self.collection.subscriber) {
        mongoc_collection_destroy(self.collection.subscriber);
        self.collection.subscriber = NULL;
    }

#if MONGOC_CHECK_VERSION(1, 9, 0)
    if (self.stream) {
        mongoc_change_stream_destroy(self.stream);
        self.stream = NULL;
    }
#endif

    if (self.database) {
        mongoc_database_destroy(self.database);
        self.database = NULL;
    }
    if (self.client) {
        mongoc_client_destroy(self.client);
        self.client = NULL;
    }
    if (self.masked_db_uri) {
        ogs_free(self.masked_db_uri);
        self.masked_db_uri = NULL;
    }

    if (self.initialized) {
        mongoc_cleanup();
        self.initialized = false;
    }
}

ogs_mongoc_t *ogs_mongoc(void)
{
    return &self;
}
```

- [ ] **Step 3: Empty out `lib/dbi/ogs-mongoc.c`**

`ogs_mongoc_init`, `ogs_mongoc_final`, and `ogs_mongoc()` have moved to `mongoc-backend.c`. The `masked_db_uri` helper and `ogs_mongoc_mongoc_client_get_server_status` helper went with them.

Replace the entire contents of `lib/dbi/ogs-mongoc.c` with a stub that exists only because `meson.build` references it (we delete the file in Task 9):

```c
/*
 * Phase-1 transition stub. Body has moved to mongoc/mongoc-backend.c.
 * This file is removed entirely in the Task-9 commit.
 */
```

- [ ] **Step 4: Make `lib/dbi/ogs-mongoc.h` a compat shim**

Replace `lib/dbi/ogs-mongoc.h` body (keep license header) with a stub that pulls in the internal types only when mongoc support is compiled in. This preserves the public symbol `ogs_mongoc()` for any out-of-tree consumer.

```c
/* license header preserved as in current file */

#if !defined(OGS_DBI_INSIDE) && !defined(OGS_DBI_COMPILATION)
#error "This header cannot be included directly."
#endif

#ifndef OGS_MONGOC_H
#define OGS_MONGOC_H

/*
 * Public compat shim. The mongoc backend's types and accessor used to
 * live here; they now live in lib/dbi/mongoc/mongoc-internal.h, which is
 * not installed. Out-of-tree callers that previously used `ogs_mongoc()`
 * keep working as long as the mongoc backend is enabled at build time.
 */

#ifdef OGS_DBI_HAVE_MONGOC
#include "mongoc/mongoc-internal.h"
#endif

#endif /* OGS_MONGOC_H */
```

- [ ] **Step 5: Update `lib/dbi/meson.build`**

Add the mongoc backend sources gated on the mongo option:

```meson
# Copyright (C) 2019-2026 by Sukchan Lee <acetcom@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

libdbi_sources = files('''
    ogs-dbi.h
    ogs-dbi-backend.h

    ogs-dbi-core.c
    subscription.c
    session.c
    ims.c
'''.split())

backend_deps = []
backend_c_args = []

libmongoc_dep = dependency('libmongoc-1.0', required: get_option('mongo'))
if libmongoc_dep.found()
    libdbi_sources += files('''
        ogs-mongoc.h
        ogs-mongoc.c

        mongoc/mongoc-internal.h
        mongoc/mongoc-backend.c
    '''.split())
    backend_deps += libmongoc_dep
    backend_c_args += '-DOGS_DBI_HAVE_MONGOC'
endif

# Phase-2 placeholder. The redis backend lands in a later phase; the
# option exists today only to surface the dependency check.
libhiredis_dep = dependency('hiredis', required: get_option('redis'))
if libhiredis_dep.found()
    backend_deps += libhiredis_dep
    backend_c_args += '-DOGS_DBI_HAVE_REDIS'
endif

if not libmongoc_dep.found() and not libhiredis_dep.found()
    error('At least one of -Dmongo / -Dredis must be enabled')
endif

libdbi_inc = include_directories('.')

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

- [ ] **Step 6: Build — expect failure with undefined references to `mongoc_auth_info`, `mongoc_subscription_data`, etc.**

Run: `meson setup build --reconfigure && meson compile -C build 2>&1 | tail -40`
Expected: link errors naming the per-method `mongoc_*` symbols declared in `mongoc-internal.h` but not yet defined. The lib/dbi/ build itself completes the `.o` files; the link happens in NF executables.

- [ ] **Step 7: Provide temporary stubs so the build can link**

Add to the end of `lib/dbi/mongoc/mongoc-backend.c`:

```c
/* ----- TEMPORARY STUBS — replaced in Tasks 7–10 ----- */

int mongoc_auth_info(char *supi, ogs_dbi_auth_info_t *out)
{ ogs_fatal("mongoc_auth_info stub called"); return OGS_ERROR; }
int mongoc_update_sqn(char *supi, uint64_t sqn)
{ ogs_fatal("mongoc_update_sqn stub called"); return OGS_ERROR; }
int mongoc_increment_sqn(char *supi)
{ ogs_fatal("mongoc_increment_sqn stub called"); return OGS_ERROR; }
int mongoc_update_imeisv(char *supi, char *imeisv)
{ ogs_fatal("mongoc_update_imeisv stub called"); return OGS_ERROR; }
int mongoc_update_mme(char *supi, char *host, char *realm, bool purge)
{ ogs_fatal("mongoc_update_mme stub called"); return OGS_ERROR; }
int mongoc_subscription_data(char *supi, ogs_subscription_data_t *out)
{ ogs_fatal("mongoc_subscription_data stub called"); return OGS_ERROR; }
int mongoc_session_data(const char *supi, const ogs_s_nssai_t *s_nssai,
        const char *dnn, ogs_session_data_t *out)
{ ogs_fatal("mongoc_session_data stub called"); return OGS_ERROR; }
int mongoc_msisdn_data(char *id, ogs_msisdn_data_t *out)
{ ogs_fatal("mongoc_msisdn_data stub called"); return OGS_ERROR; }
int mongoc_ims_data(char *supi, ogs_ims_data_t *out)
{ ogs_fatal("mongoc_ims_data stub called"); return OGS_ERROR; }
int mongoc_watch_init(void)
{ ogs_fatal("mongoc_watch_init stub called"); return OGS_ERROR; }
int mongoc_poll_change_stream(void)
{ ogs_fatal("mongoc_poll_change_stream stub called"); return OGS_ERROR; }
```

These are removed in the matching tasks below as each real implementation lands. The `ogs_fatal` calls mean any actual run-time invocation will assert; we rely on dbi-tests not exercising these (registry tests use the stub_backend).

- [ ] **Step 8: Build successfully**

Run: `meson compile -C build`
Expected: clean build. All NF executables link.

- [ ] **Step 9: Run all existing tests except those that need a live MongoDB**

Run: `meson test -C build --suite unit --suite dbi -v`
Expected: all pass.

- [ ] **Step 10: Commit**

```bash
git add lib/dbi/mongoc/ lib/dbi/ogs-mongoc.c lib/dbi/ogs-mongoc.h \
        lib/dbi/meson.build
git commit -m "dbi: introduce mongoc/ backend skeleton with vtable registration"
```

---

## Task 7: Move `subscription.c` body into `mongoc/mongoc-subscription.c`

**Files:**
- Create: `lib/dbi/mongoc/mongoc-subscription.c`
- Modify: `lib/dbi/mongoc/mongoc-backend.c` (delete the matching stubs)
- Modify: `lib/dbi/meson.build` (add new source)
- (Will be modified in Task 10: `lib/dbi/subscription.c` becomes the dispatcher)

- [ ] **Step 1: Create `lib/dbi/mongoc/mongoc-subscription.c` by copying the current `lib/dbi/subscription.c`**

```bash
cp lib/dbi/subscription.c lib/dbi/mongoc/mongoc-subscription.c
```

- [ ] **Step 2: Edit `lib/dbi/mongoc/mongoc-subscription.c`**

In `lib/dbi/mongoc/mongoc-subscription.c`:

1. Replace `#include "ogs-dbi.h"` with `#include "mongoc-internal.h"`.
2. Rename function definitions:
   - `int ogs_dbi_auth_info(` → `int mongoc_auth_info(`
   - `int ogs_dbi_update_sqn(` → `int mongoc_update_sqn(`
   - `int ogs_dbi_increment_sqn(` → `int mongoc_increment_sqn(`
   - `int ogs_dbi_update_imeisv(` → `int mongoc_update_imeisv(`
   - `int ogs_dbi_update_mme(` → `int mongoc_update_mme(`
   - `int ogs_dbi_subscription_data(` → `int mongoc_subscription_data(`
3. Calls to other `ogs_dbi_*` helpers within this file (e.g. internal calls — there are none in this file currently) stay unchanged.
4. References to `ogs_mongoc()->collection.subscriber` stay unchanged — that helper now lives in the mongoc backend's `self` and is still reached via `ogs_mongoc()`.

- [ ] **Step 3: Delete the matching stubs from `lib/dbi/mongoc/mongoc-backend.c`**

Remove the five stub function definitions (`mongoc_auth_info`, `mongoc_update_sqn`, `mongoc_increment_sqn`, `mongoc_update_imeisv`, `mongoc_update_mme`, `mongoc_subscription_data`) added in Task 6 Step 7.

- [ ] **Step 4: Add the new source to `libdbi_sources` in `lib/dbi/meson.build`**

Inside the `if libmongoc_dep.found()` block, add `mongoc/mongoc-subscription.c`:

```meson
libdbi_sources += files('''
    ogs-mongoc.h
    ogs-mongoc.c

    mongoc/mongoc-internal.h
    mongoc/mongoc-backend.c
    mongoc/mongoc-subscription.c
'''.split())
```

- [ ] **Step 5: Build**

Run: `meson compile -C build`
Expected: clean build. NF executables still link (they call `ogs_dbi_auth_info` etc., which are still defined in the unchanged `lib/dbi/subscription.c`; that file is rewritten in Task 10).

- [ ] **Step 6: Run tests**

Run: `meson test -C build --suite unit --suite dbi -v`
Expected: all pass.

- [ ] **Step 7: Commit**

```bash
git add lib/dbi/mongoc/mongoc-subscription.c lib/dbi/mongoc/mongoc-backend.c \
        lib/dbi/meson.build
git commit -m "dbi: move subscription.c body into mongoc/mongoc-subscription.c"
```

---

## Task 8: Move `session.c` body into `mongoc/mongoc-session.c`

**Files:**
- Create: `lib/dbi/mongoc/mongoc-session.c`
- Modify: `lib/dbi/mongoc/mongoc-backend.c` (delete the `mongoc_session_data` stub)
- Modify: `lib/dbi/meson.build`

- [ ] **Step 1: Copy and edit**

```bash
cp lib/dbi/session.c lib/dbi/mongoc/mongoc-session.c
```

In `lib/dbi/mongoc/mongoc-session.c`:
1. Replace `#include "ogs-dbi.h"` with `#include "mongoc-internal.h"`.
2. Rename `int ogs_dbi_session_data(` → `int mongoc_session_data(`.

- [ ] **Step 2: Delete `mongoc_session_data` stub from `mongoc-backend.c`**

- [ ] **Step 3: Add source to meson**

Append `mongoc/mongoc-session.c` to the `libdbi_sources` list inside the `if libmongoc_dep.found()` block.

- [ ] **Step 4: Build and test**

Run: `meson compile -C build && meson test -C build --suite unit --suite dbi -v`
Expected: pass.

- [ ] **Step 5: Commit**

```bash
git add lib/dbi/mongoc/mongoc-session.c lib/dbi/mongoc/mongoc-backend.c \
        lib/dbi/meson.build
git commit -m "dbi: move session.c body into mongoc/mongoc-session.c"
```

---

## Task 9: Move `ims.c` body into `mongoc/mongoc-ims.c` and delete `ogs-mongoc.c`

**Files:**
- Create: `lib/dbi/mongoc/mongoc-ims.c`
- Delete: `lib/dbi/ogs-mongoc.c`
- Modify: `lib/dbi/mongoc/mongoc-backend.c` (delete `mongoc_msisdn_data` and `mongoc_ims_data` stubs)
- Modify: `lib/dbi/meson.build`

- [ ] **Step 1: Copy and edit**

```bash
cp lib/dbi/ims.c lib/dbi/mongoc/mongoc-ims.c
```

In `lib/dbi/mongoc/mongoc-ims.c`:
1. Replace `#include "ogs-dbi.h"` with `#include "mongoc-internal.h"`.
2. Rename `int ogs_dbi_msisdn_data(` → `int mongoc_msisdn_data(`.
3. Rename `int ogs_dbi_ims_data(` → `int mongoc_ims_data(`.

- [ ] **Step 2: Delete the two matching stubs from `mongoc-backend.c`**

- [ ] **Step 3: Delete `lib/dbi/ogs-mongoc.c`**

```bash
git rm lib/dbi/ogs-mongoc.c
```

- [ ] **Step 4: Update `lib/dbi/meson.build`**

Remove `ogs-mongoc.c` from the source list and add `mongoc/mongoc-ims.c`:

```meson
libdbi_sources += files('''
    ogs-mongoc.h

    mongoc/mongoc-internal.h
    mongoc/mongoc-backend.c
    mongoc/mongoc-subscription.c
    mongoc/mongoc-session.c
    mongoc/mongoc-ims.c
'''.split())
```

- [ ] **Step 5: Build and test**

Run: `meson compile -C build && meson test -C build --suite unit --suite dbi -v`
Expected: pass.

- [ ] **Step 6: Commit**

```bash
git add lib/dbi/mongoc/mongoc-ims.c lib/dbi/mongoc/mongoc-backend.c \
        lib/dbi/meson.build lib/dbi/ogs-mongoc.c
git commit -m "dbi: move ims.c body into mongoc/mongoc-ims.c; drop ogs-mongoc.c"
```

---

## Task 10: Rewrite `subscription.c`, `session.c`, `ims.c` as thin dispatchers

**Files:**
- Modify (rewrite): `lib/dbi/subscription.c`
- Modify (rewrite): `lib/dbi/session.c`
- Modify (rewrite): `lib/dbi/ims.c`

- [ ] **Step 1: Replace `lib/dbi/subscription.c` entirely**

```c
/*
 * Copyright (C) 2019-2026 by Sukchan Lee <acetcom@gmail.com>
 *
 * This file is part of Open5GS.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Thin vtable dispatcher. The backend implementations live in
 * lib/dbi/<backend>/<backend>-subscription.c.
 */

#define OGS_DBI_COMPILATION
#include "ogs-dbi.h"
#include "ogs-dbi-backend.h"

int ogs_dbi_auth_info(char *supi, ogs_dbi_auth_info_t *auth_info)
{
    const ogs_dbi_backend_t *b = ogs_dbi_current_backend();
    ogs_assert(b);
    return b->auth_info(supi, auth_info);
}

int ogs_dbi_update_sqn(char *supi, uint64_t sqn)
{
    const ogs_dbi_backend_t *b = ogs_dbi_current_backend();
    ogs_assert(b);
    return b->update_sqn(supi, sqn);
}

int ogs_dbi_increment_sqn(char *supi)
{
    const ogs_dbi_backend_t *b = ogs_dbi_current_backend();
    ogs_assert(b);
    return b->increment_sqn(supi);
}

int ogs_dbi_update_imeisv(char *supi, char *imeisv)
{
    const ogs_dbi_backend_t *b = ogs_dbi_current_backend();
    ogs_assert(b);
    return b->update_imeisv(supi, imeisv);
}

int ogs_dbi_update_mme(char *supi, char *mme_host, char *mme_realm,
                       bool purge_flag)
{
    const ogs_dbi_backend_t *b = ogs_dbi_current_backend();
    ogs_assert(b);
    return b->update_mme(supi, mme_host, mme_realm, purge_flag);
}

int ogs_dbi_subscription_data(char *supi,
                              ogs_subscription_data_t *subscription_data)
{
    const ogs_dbi_backend_t *b = ogs_dbi_current_backend();
    ogs_assert(b);
    return b->subscription_data(supi, subscription_data);
}
```

- [ ] **Step 2: Replace `lib/dbi/session.c` entirely**

```c
/*
 * Copyright (C) 2019-2026 by Sukchan Lee <acetcom@gmail.com>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#define OGS_DBI_COMPILATION
#include "ogs-dbi.h"
#include "ogs-dbi-backend.h"

int ogs_dbi_session_data(
        const char *supi, const ogs_s_nssai_t *s_nssai, const char *dnn,
        ogs_session_data_t *session_data)
{
    const ogs_dbi_backend_t *b = ogs_dbi_current_backend();
    ogs_assert(b);
    return b->session_data(supi, s_nssai, dnn, session_data);
}
```

- [ ] **Step 3: Replace `lib/dbi/ims.c` entirely**

```c
/*
 * Copyright (C) 2019-2026 by Sukchan Lee <acetcom@gmail.com>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#define OGS_DBI_COMPILATION
#include "ogs-dbi.h"
#include "ogs-dbi-backend.h"

int ogs_dbi_msisdn_data(char *imsi_or_msisdn_bcd,
                        ogs_msisdn_data_t *msisdn_data)
{
    const ogs_dbi_backend_t *b = ogs_dbi_current_backend();
    ogs_assert(b);
    return b->msisdn_data(imsi_or_msisdn_bcd, msisdn_data);
}

int ogs_dbi_ims_data(char *supi, ogs_ims_data_t *ims_data)
{
    const ogs_dbi_backend_t *b = ogs_dbi_current_backend();
    ogs_assert(b);
    return b->ims_data(supi, ims_data);
}
```

- [ ] **Step 4: Build and test**

Run: `meson compile -C build && meson test -C build --suite unit --suite dbi -v`
Expected: pass.

- [ ] **Step 5: Sanity-check that nothing in `src/` still references mongoc-specific symbols from the dbi side**

Run: `grep -rln "mongoc_collection_\|bson_iter_\|bson_t\b" src/ lib/dbi/*.c 2>/dev/null`
Expected: only `src/hss/hss-context.c` and `src/hss/hss-sm.c` (those get fixed in Tasks 13–14). No matches in `lib/dbi/*.c` (the dispatchers). All other matches must be inside `lib/dbi/mongoc/`.

- [ ] **Step 6: Commit**

```bash
git add lib/dbi/subscription.c lib/dbi/session.c lib/dbi/ims.c
git commit -m "dbi: rewrite subscription/session/ims.c as vtable dispatchers"
```

---

## Task 11: Move change-stream code into `mongoc/mongoc-watch.c` (without HSS edits yet)

**Files:**
- Create: `lib/dbi/mongoc/mongoc-watch.c`
- Modify: `lib/dbi/mongoc/mongoc-backend.c` (delete watch stubs)
- Modify: `lib/dbi/meson.build`

This task only moves code; the HSS change-event handler is still consuming a `bson_t *` after this task (we fix that in Tasks 12–14).

- [ ] **Step 1: Create `lib/dbi/mongoc/mongoc-watch.c`**

```c
/*
 * Copyright (C) 2026 by Sukchan Lee <acetcom@gmail.com>
 *
 * This file is part of Open5GS.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * MongoDB change-stream watcher. Opens a change stream on the subscribers
 * collection in mongoc_watch_init(), polls for events in
 * mongoc_poll_change_stream(), and converts each event into a
 * backend-neutral ogs_dbi_change_event_t which is delivered to HSS via
 * the HSS event bus.
 */

#include "mongoc-internal.h"

int mongoc_watch_init(void)
{
#if MONGOC_CHECK_VERSION(1, 9, 0)
    bson_t empty = BSON_INITIALIZER;
    const bson_t *err_doc;
    bson_error_t error;
    bson_t *options = BCON_NEW("fullDocument", "updateLookup");

    ogs_mongoc()->stream = mongoc_collection_watch(
        ogs_mongoc()->collection.subscriber, &empty, options);

    if (mongoc_change_stream_error_document(
            ogs_mongoc()->stream, &error, &err_doc)) {
        if (!bson_empty(err_doc)) {
            ogs_error("Change Stream Error. Enable replica sets to enable "
                      "database updates to be sent to MME.");
        } else {
            ogs_error("Client Error: %s\n", error.message);
        }
        return OGS_ERROR;
    }
    ogs_info("MongoDB Change Streams are Enabled.");
    return OGS_OK;
#else
    return OGS_ERROR;
#endif
}

/*
 * Phase 1 retains the original poll behavior: read change-stream events,
 * hand them to the HSS via the existing event-bus protocol. The BSON →
 * neutral struct conversion lands in Task 12, when the HSS interface
 * changes. For this commit, the function body is identical to the old
 * code that previously lived in src/hss/hss-context.c's
 * process_change_stream().
 *
 * NOTE: this temporary cross-layer dependency (a backend file calling
 * `hss_handle_change_event`) is removed in Task 12 — we will replace
 * the direct call here with an event-bus push of a neutral struct.
 */
int mongoc_poll_change_stream(void)
{
#if MONGOC_CHECK_VERSION(1, 9, 0)
    const bson_t *document;
    const bson_t *err_document;
    bson_error_t error;

    if (!ogs_mongoc()->stream)
        return OGS_ERROR;

    while (mongoc_change_stream_next(ogs_mongoc()->stream, &document)) {
        /* In Task 12 this becomes:
         *   ogs_dbi_change_event_t *ev = bson_to_change_event(document);
         *   hss_event_push(HSS_EVENT_DBI_MESSAGE, ev);
         * For now, preserve the existing behavior (which the HSS still
         * consumes via the cross-layer call). */
        extern int hss_dbi_event_push_bson(const bson_t *);
        hss_dbi_event_push_bson(document);
    }

    if (mongoc_change_stream_error_document(
            ogs_mongoc()->stream, &error, &err_document)) {
        if (!bson_empty(err_document)) {
            ogs_error("Server Error: %s\n",
                bson_as_relaxed_extended_json(err_document, NULL));
        } else {
            ogs_error("Client Error: %s\n", error.message);
        }
        return OGS_ERROR;
    }
    return OGS_OK;
#else
    return OGS_ERROR;
#endif
}
```

- [ ] **Step 2: Provide the `hss_dbi_event_push_bson` shim in `src/hss/hss-context.c`**

This is a short-lived shim. Wraps the old logic that lived inline in `process_change_stream` and `hss_handle_change_event`. We delete it in Task 12.

Find the `process_change_stream` function in `src/hss/hss-context.c` (around line 1411). Replace its declaration and body with:

```c
int hss_dbi_event_push_bson(const bson_t *document)
{
    hss_event_t *e = hss_event_new(HSS_EVENT_DBI_MESSAGE);
    int rv;

    ogs_assert(e);
    e->dbi.document = bson_copy(document);

    rv = ogs_queue_push(ogs_app()->queue, e);
    if (rv != OGS_OK) {
        ogs_error("ogs_queue_push() failed: %d", rv);
        bson_destroy(e->dbi.document);
        hss_event_free(e);
    }
    return rv;
}
```

Also remove the now-unused inline `process_change_stream` static prototype near `src/hss/hss-context.c:1365`.

Search for any other call to `process_change_stream` in `src/hss/`: there should be none after the removal (it was static and only called from `hss_db_poll_change_stream` which itself is gone — confirm by grep).

- [ ] **Step 3: Remove `hss_db_poll_change_stream` from `src/hss/hss-context.c` (lines around 1380-1400)**

The polling loop now lives inside `mongoc_poll_change_stream` in the backend. The HSS timer (`hss-timer.c:80`) calls `hss_db_poll_change_stream()`; change that call to `ogs_dbi_poll_change_stream()`.

In `src/hss/hss-timer.c` (line 80), change:

```c
hss_db_poll_change_stream();
```

to:

```c
ogs_dbi_poll_change_stream();
```

And remove the `hss_db_poll_change_stream` declaration in `src/hss/hss-context.h` if present, plus its definition body in `hss-context.c`.

- [ ] **Step 4: Delete the watch stubs from `mongoc-backend.c`**

Remove `mongoc_watch_init` and `mongoc_poll_change_stream` stub definitions from `lib/dbi/mongoc/mongoc-backend.c`.

- [ ] **Step 5: Update `lib/dbi/meson.build`**

Add `mongoc/mongoc-watch.c` to the `libdbi_sources` list inside the `if libmongoc_dep.found()` block.

- [ ] **Step 6: Build and test**

Run: `meson compile -C build && meson test -C build --suite unit --suite dbi -v`
Expected: pass.

- [ ] **Step 7: Commit**

```bash
git add lib/dbi/mongoc/mongoc-watch.c lib/dbi/mongoc/mongoc-backend.c \
        lib/dbi/meson.build src/hss/hss-context.c src/hss/hss-context.h \
        src/hss/hss-timer.c
git commit -m "dbi: relocate change-stream loop into mongoc/mongoc-watch.c

Introduces a temporary hss_dbi_event_push_bson shim used by the
mongoc backend to publish change events to the HSS event bus. The
shim is replaced with a backend-neutral push in the next commit."
```

---

## Task 12: Translate BSON → neutral `ogs_dbi_change_event_t` in `mongoc-watch.c`

**Files:**
- Modify: `lib/dbi/mongoc/mongoc-watch.c`

This task introduces `mongoc_build_change_event` — the BSON-walker, moved from `src/hss/hss-context.c:1432` — and changes `mongoc_poll_change_stream` to push the neutral struct.

- [ ] **Step 1: Add `mongoc_build_change_event` to `lib/dbi/mongoc/mongoc-watch.c`**

Insert before `mongoc_poll_change_stream`:

```c
/*
 * Walk a MongoDB change-stream document and produce a backend-neutral
 * ogs_dbi_change_event_t. Returns NULL if the document does not have a
 * full subscriber view (e.g. delete events with no fullDocument).
 *
 * Field-name → mask mapping must stay in sync with the consumer in
 * src/hss/hss-context.c:hss_handle_change_event(). Treat
 * OGS_DBI_FIELD_ALL as the safe fallback if a new HSS-watched field
 * appears here without a matching bit.
 */
static ogs_dbi_change_event_t *mongoc_build_change_event(
        const bson_t *document)
{
    bson_iter_t iter, child1_iter, child2_iter;
    char *imsi_bcd = NULL;
    uint32_t mask = 0;
    const char *utf8;
    uint32_t length;
    ogs_dbi_change_event_t *event;

    if (!bson_iter_init_find(&iter, document, "fullDocument"))
        return NULL;

    bson_iter_recurse(&iter, &child1_iter);
    while (bson_iter_next(&child1_iter)) {
        const char *key = bson_iter_key(&child1_iter);
        if (!strcmp(key, "imsi") && BSON_ITER_HOLDS_UTF8(&child1_iter)) {
            utf8 = bson_iter_utf8(&child1_iter, &length);
            imsi_bcd = ogs_strndup(utf8,
                ogs_min(length, OGS_MAX_IMSI_BCD_LEN) + 1);
        }
    }

    if (!imsi_bcd)
        return NULL;

    if (bson_iter_init_find(&iter, document, "updateDescription")) {
        bson_iter_recurse(&iter, &child1_iter);
        while (bson_iter_next(&child1_iter)) {
            if (strcmp(bson_iter_key(&child1_iter), "updatedFields") != 0 ||
                    !BSON_ITER_HOLDS_DOCUMENT(&child1_iter))
                continue;

            bson_iter_recurse(&child1_iter, &child2_iter);
            while (bson_iter_next(&child2_iter)) {
                const char *k = bson_iter_key(&child2_iter);

                if (!strcmp(k, "request_cancel_location") &&
                        BSON_ITER_HOLDS_BOOL(&child2_iter) &&
                        bson_iter_bool(&child2_iter)) {
                    mask |= OGS_DBI_FIELD_REQUEST_CANCEL_LOCATION;
                } else if (!strncmp(k, OGS_ACCESS_RESTRICTION_DATA_STRING,
                            strlen(OGS_ACCESS_RESTRICTION_DATA_STRING))) {
                    mask |= OGS_DBI_FIELD_ACCESS_RESTRICTION_DATA;
                } else if (!strncmp(k, OGS_SUBSCRIBER_STATUS_STRING,
                            strlen(OGS_SUBSCRIBER_STATUS_STRING))) {
                    mask |= OGS_DBI_FIELD_SUBSCRIBER_STATUS;
                } else if (!strncmp(k,
                            OGS_OPERATOR_DETERMINED_BARRING_STRING,
                            strlen(OGS_OPERATOR_DETERMINED_BARRING_STRING))) {
                    mask |= OGS_DBI_FIELD_OP_DETERMINED_BARRING;
                } else if (!strncmp(k, OGS_NETWORK_ACCESS_MODE_STRING,
                            strlen(OGS_NETWORK_ACCESS_MODE_STRING))) {
                    mask |= OGS_DBI_FIELD_NETWORK_ACCESS_MODE;
                } else if (!strncmp(k, "ambr", strlen("ambr"))) {
                    mask |= OGS_DBI_FIELD_AMBR;
                } else if (!strncmp(k, OGS_SUBSCRIBED_RAU_TAU_TIMER_STRING,
                            strlen(OGS_SUBSCRIBED_RAU_TAU_TIMER_STRING))) {
                    mask |= OGS_DBI_FIELD_RAU_TAU_TIMER;
                } else if (!strncmp(k, "slice", strlen("slice"))) {
                    mask |= OGS_DBI_FIELD_SLICE;
                }
            }
        }
    } else {
        /* On insert / replace, "updateDescription" is absent. Treat as
         * "all fields might have changed" to match the conservative S6a
         * behavior the existing HSS code already followed in that case. */
        mask = OGS_DBI_FIELD_ALL;
    }

    event = ogs_dbi_change_event_alloc(imsi_bcd, mask);
    ogs_free(imsi_bcd);
    return event;
}
```

- [ ] **Step 2: Replace the body of `mongoc_poll_change_stream` to push the neutral struct**

```c
int mongoc_poll_change_stream(void)
{
#if MONGOC_CHECK_VERSION(1, 9, 0)
    const bson_t *document;
    const bson_t *err_document;
    bson_error_t error;
    extern int hss_dbi_event_push(ogs_dbi_change_event_t *);

    if (!ogs_mongoc()->stream)
        return OGS_ERROR;

    while (mongoc_change_stream_next(ogs_mongoc()->stream, &document)) {
        ogs_dbi_change_event_t *ev = mongoc_build_change_event(document);
        if (!ev) {
            ogs_warn("mongoc change event with no IMSI field, skipping");
            continue;
        }
        if (hss_dbi_event_push(ev) != OGS_OK) {
            ogs_dbi_change_event_free(ev);
        }
    }

    if (mongoc_change_stream_error_document(
            ogs_mongoc()->stream, &error, &err_document)) {
        if (!bson_empty(err_document)) {
            ogs_error("Server Error: %s\n",
                bson_as_relaxed_extended_json(err_document, NULL));
        } else {
            ogs_error("Client Error: %s\n", error.message);
        }
        return OGS_ERROR;
    }
    return OGS_OK;
#else
    return OGS_ERROR;
#endif
}
```

- [ ] **Step 3: Replace the bson-flavored shim in HSS with a neutral one**

In `src/hss/hss-context.c`, replace `hss_dbi_event_push_bson` with `hss_dbi_event_push`:

```c
int hss_dbi_event_push(ogs_dbi_change_event_t *change_event)
{
    hss_event_t *e = hss_event_new(HSS_EVENT_DBI_MESSAGE);
    int rv;

    ogs_assert(e);
    e->dbi.change_event = change_event;   /* dbi.change_event is a void * */

    rv = ogs_queue_push(ogs_app()->queue, e);
    if (rv != OGS_OK) {
        ogs_error("ogs_queue_push() failed: %d", rv);
        e->dbi.change_event = NULL;
        hss_event_free(e);
        return rv;
    }
    return OGS_OK;
}
```

Note: `dbi.change_event` is the new field name; we rename it in Task 13. For this commit only, keep using `dbi.document` (the existing name in `hss-event.h`) and rename in Task 13. The line above is corrected in Task 13.

For this task, use the existing field name:

```c
    e->dbi.document = change_event;       /* still void *, holds neutral struct */
```

- [ ] **Step 4: Build and test**

Run: `meson compile -C build && meson test -C build --suite unit --suite dbi -v`
Expected: pass. (`hss_handle_change_event` still consumes a `bson_t *`; the queued events now hold `ogs_dbi_change_event_t *` instead. The HSS event-bus consumer is fixed in Task 13.)

**Wait — actually this won't link.** `hss_handle_change_event(e->dbi.document)` in `hss-sm.c` is called on what is now an `ogs_dbi_change_event_t *` but expects `const bson_t *`. The cast won't crash at link time (both are `void *` on the bus), but the BSON-walker inside `hss_handle_change_event` will read garbage at runtime.

**Resolution:** Tasks 12 and 13 must land together in a single commit OR Task 12 must keep pushing bson_t until Task 13 lands.

**Plan revision:** Defer Step 3 of this task; keep the bson-flavored shim until Task 13. So Step 3 becomes:

- [ ] **Step 3 (revised): Leave `hss_dbi_event_push_bson` in place; rename it to `hss_dbi_event_push` AND change the type in Task 13.**

For now, change `mongoc_poll_change_stream` to *not yet* call the neutral push; keep calling `hss_dbi_event_push_bson(document)` directly. So this task introduces `mongoc_build_change_event` but does not yet wire it up — it sits unused until Task 13.

Update Step 2 above to leave `mongoc_poll_change_stream` calling `hss_dbi_event_push_bson(document)` as in Task 11. Replace Step 2 with:

```c
int mongoc_poll_change_stream(void)
{
#if MONGOC_CHECK_VERSION(1, 9, 0)
    const bson_t *document;
    const bson_t *err_document;
    bson_error_t error;
    extern int hss_dbi_event_push_bson(const bson_t *);

    if (!ogs_mongoc()->stream)
        return OGS_ERROR;

    while (mongoc_change_stream_next(ogs_mongoc()->stream, &document)) {
        hss_dbi_event_push_bson(document);
    }

    if (mongoc_change_stream_error_document(
            ogs_mongoc()->stream, &error, &err_document)) {
        if (!bson_empty(err_document)) {
            ogs_error("Server Error: %s\n",
                bson_as_relaxed_extended_json(err_document, NULL));
        } else {
            ogs_error("Client Error: %s\n", error.message);
        }
        return OGS_ERROR;
    }
    return OGS_OK;
#else
    return OGS_ERROR;
#endif
}
```

Suppress the "unused static function" warning on `mongoc_build_change_event` by either marking it `__attribute__((unused))` or by adding a TODO comment and accepting the compiler warning. Prefer the attribute so the build stays clean:

```c
__attribute__((unused))
static ogs_dbi_change_event_t *mongoc_build_change_event(...)
```

- [ ] **Step 4 (revised): Build and test**

Run: `meson compile -C build && meson test -C build --suite unit --suite dbi -v`
Expected: pass. No behavior change from Task 11.

- [ ] **Step 5: Commit**

```bash
git add lib/dbi/mongoc/mongoc-watch.c
git commit -m "dbi(mongoc): add bson-to-neutral change-event builder

mongoc_build_change_event is wired in by Task 13 after the HSS-side
consumer is converted to take ogs_dbi_change_event_t."
```

---

## Task 13: Convert HSS to consume neutral `ogs_dbi_change_event_t`

**Files:**
- Modify: `src/hss/hss-event.h` (rename `dbi.document` → `dbi.change_event`)
- Modify: `src/hss/hss-context.h` (change `hss_handle_change_event` signature)
- Modify: `src/hss/hss-context.c` (rewrite the handler; update shim signature)
- Modify: `src/hss/hss-sm.c` (use `ogs_dbi_change_event_free`)
- Modify: `lib/dbi/mongoc/mongoc-watch.c` (wire `mongoc_build_change_event` in; rename shim call)

- [ ] **Step 1: Rename the union field in `src/hss/hss-event.h`**

Change `lib/.../hss-event.h:42-44`:

```c
struct {
    void *document;
} dbi;
```

to:

```c
struct {
    void *change_event; /* ogs_dbi_change_event_t * */
} dbi;
```

- [ ] **Step 2: Update `src/hss/hss-context.h`**

Change the prototype near line 98:

```c
int hss_handle_change_event(const bson_t *document);
```

to:

```c
int hss_handle_change_event(const ogs_dbi_change_event_t *event);
```

Remove the `#include` for `mongoc.h`/`bson.h` from `hss-context.h` if present (find with grep). They are no longer needed by the HSS public surface.

Run: `grep -n "bson\|mongoc" src/hss/hss-context.h`
Action: remove any matching include lines.

- [ ] **Step 3: Rewrite `hss_handle_change_event` in `src/hss/hss-context.c`**

Locate the existing body at `hss-context.c:1432`. Replace it entirely with:

```c
int hss_handle_change_event(const ogs_dbi_change_event_t *event)
{
    bool send_clr_flag;
    bool send_idr_flag;
    uint32_t subdatamask = 0;

    ogs_assert(event);
    ogs_assert(event->imsi_bcd);

    ogs_debug("Received change event for IMSI[%s], fields[0x%08x]",
              event->imsi_bcd, event->updated_fields_mask);

    send_clr_flag = (event->updated_fields_mask &
                     OGS_DBI_FIELD_REQUEST_CANCEL_LOCATION) != 0;

    if (event->updated_fields_mask & OGS_DBI_FIELD_ACCESS_RESTRICTION_DATA)
        subdatamask |= OGS_DIAM_S6A_SUBDATA_ARD;
    if (event->updated_fields_mask & OGS_DBI_FIELD_SUBSCRIBER_STATUS)
        subdatamask |= OGS_DIAM_S6A_SUBDATA_SUB_STATUS;
    if (event->updated_fields_mask & OGS_DBI_FIELD_OP_DETERMINED_BARRING)
        subdatamask |= OGS_DIAM_S6A_SUBDATA_OP_DET_BARRING;
    if (event->updated_fields_mask & OGS_DBI_FIELD_NETWORK_ACCESS_MODE)
        subdatamask |= OGS_DIAM_S6A_SUBDATA_NAM;
    if (event->updated_fields_mask & OGS_DBI_FIELD_AMBR)
        subdatamask |= OGS_DIAM_S6A_SUBDATA_UEAMBR;
    if (event->updated_fields_mask & OGS_DBI_FIELD_RAU_TAU_TIMER)
        subdatamask |= OGS_DIAM_S6A_SUBDATA_RAU_TAU_TIMER;
    if (event->updated_fields_mask & OGS_DBI_FIELD_SLICE) {
        /* Reuse whichever subdata bit(s) the previous code set for
         * "slice"-prefixed fields. Inspect the original
         * src/hss/hss-context.c body (pre-Task-13) to confirm which bits
         * the slice branch set. Document the bit choices here. */
        /* As of the time of this plan, the original code set neither
         * SUB_STATUS nor specific subdata bits for slice changes — it
         * fell through to the catch-all `send_idr_flag = true`. We mark
         * the slice branch as needing a full IDR; if a more specific bit
         * is appropriate in a follow-up, update both this map and the
         * BSON walker in mongoc-watch.c together. */
        subdatamask |= OGS_DIAM_S6A_SUBDATA_APN_CONFIG;
    }

    send_idr_flag = (subdatamask != 0);

    /*
     * Reuse the existing post-mask logic from the deleted body. Copy
     * from the original src/hss/hss-context.c (anything after the inner
     * `while (bson_iter_next(&child2_iter))` loop that uses
     * imsi_bcd / send_clr_flag / send_idr_flag / subdatamask) into here
     * verbatim, replacing imsi_bcd with event->imsi_bcd.
     */
    /* ... see the deleted body in `git show <Task-12 commit>^:src/hss/hss-context.c` for the verbatim block ... */

    return OGS_OK;
}
```

**Critical:** the original `hss_handle_change_event` body after the BSON-walking loop contained the actual S6a IDR/CLR dispatch logic. That logic must be preserved verbatim — only the field-extraction prologue changes. Before editing, run:

```bash
git show HEAD:src/hss/hss-context.c | sed -n '1432,1540p'
```

Read that block. The new function's tail (after computing `subdatamask` and the two flags) must match it line-for-line, with `imsi_bcd` replaced by `event->imsi_bcd`.

- [ ] **Step 4: Rename the shim and change its type in `hss-context.c`**

Find `hss_dbi_event_push_bson` and replace with:

```c
int hss_dbi_event_push(ogs_dbi_change_event_t *change_event)
{
    hss_event_t *e = hss_event_new(HSS_EVENT_DBI_MESSAGE);
    int rv;

    ogs_assert(e);
    ogs_assert(change_event);
    e->dbi.change_event = change_event;

    rv = ogs_queue_push(ogs_app()->queue, e);
    if (rv != OGS_OK) {
        ogs_error("ogs_queue_push() failed: %d", rv);
        e->dbi.change_event = NULL;
        hss_event_free(e);
        ogs_dbi_change_event_free(change_event);
        return rv;
    }
    return OGS_OK;
}
```

Remove any forward declaration of `hss_dbi_event_push_bson` from `hss-context.h` and replace with:

```c
int hss_dbi_event_push(ogs_dbi_change_event_t *change_event);
```

(Or keep it internal — it's only called from `mongoc-watch.c`. Put the declaration in `hss-context.h` for now; a future cleanup might move it to a more private spot.)

- [ ] **Step 5: Update `src/hss/hss-sm.c`**

Replace lines around 92–96:

```c
    case HSS_EVENT_DBI_MESSAGE:
        ogs_assert(e);

        ogs_assert(e->dbi.document);
        hss_handle_change_event(e->dbi.document);

        bson_destroy(e->dbi.document);
        break;
```

with:

```c
    case HSS_EVENT_DBI_MESSAGE:
        ogs_assert(e);

        ogs_assert(e->dbi.change_event);
        hss_handle_change_event(
                (const ogs_dbi_change_event_t *) e->dbi.change_event);

        ogs_dbi_change_event_free(
                (ogs_dbi_change_event_t *) e->dbi.change_event);
        break;
```

- [ ] **Step 6: Wire `mongoc_build_change_event` into `mongoc_poll_change_stream`**

In `lib/dbi/mongoc/mongoc-watch.c`:
1. Remove the `__attribute__((unused))` attribute from `mongoc_build_change_event`.
2. Replace the `mongoc_poll_change_stream` body's inner loop to call `mongoc_build_change_event` and push via the new shim:

```c
int mongoc_poll_change_stream(void)
{
#if MONGOC_CHECK_VERSION(1, 9, 0)
    const bson_t *document;
    const bson_t *err_document;
    bson_error_t error;
    extern int hss_dbi_event_push(ogs_dbi_change_event_t *);

    if (!ogs_mongoc()->stream)
        return OGS_ERROR;

    while (mongoc_change_stream_next(ogs_mongoc()->stream, &document)) {
        ogs_dbi_change_event_t *ev = mongoc_build_change_event(document);
        if (!ev) {
            ogs_warn("mongoc change event with no IMSI field, skipping");
            continue;
        }
        /* hss_dbi_event_push takes ownership; frees on failure. */
        (void) hss_dbi_event_push(ev);
    }

    if (mongoc_change_stream_error_document(
            ogs_mongoc()->stream, &error, &err_document)) {
        if (!bson_empty(err_document)) {
            ogs_error("Server Error: %s\n",
                bson_as_relaxed_extended_json(err_document, NULL));
        } else {
            ogs_error("Client Error: %s\n", error.message);
        }
        return OGS_ERROR;
    }
    return OGS_OK;
#else
    return OGS_ERROR;
#endif
}
```

- [ ] **Step 7: Build**

Run: `meson compile -C build 2>&1 | tail -30`
Expected: clean build.

If you see errors like `'hss_dbi_event_push_bson' undeclared` from `lib/dbi/mongoc/mongoc-watch.c`, the `extern` line in `mongoc_poll_change_stream` needs updating from `_push_bson` to `_push`.

- [ ] **Step 8: Run unit tests**

Run: `meson test -C build --suite unit --suite dbi -v`
Expected: pass.

- [ ] **Step 9: Run all tests that don't need a live DB**

Run: `meson test -C build -v 2>&1 | tail -50`
Expected: all suites pass that don't require a running MongoDB. (Integration suites that talk to a real MongoDB may need DB setup — those are the ones gated on the migration verification in Task 15.)

- [ ] **Step 10: Commit**

```bash
git add src/hss/hss-event.h src/hss/hss-context.h src/hss/hss-context.c \
        src/hss/hss-sm.c lib/dbi/mongoc/mongoc-watch.c
git commit -m "hss: consume ogs_dbi_change_event_t instead of bson_t

The HSS no longer talks to MongoDB types directly. The Mongo backend
translates change-stream documents into a backend-neutral event in
mongoc-watch.c before pushing to the HSS event bus."
```

---

## Task 14: Verify no stray BSON / mongoc symbols remain outside `lib/dbi/mongoc/`

**Files:**
- Read-only verification

- [ ] **Step 1: Grep for any remaining direct mongoc/bson use in `src/` or in `lib/dbi/*.c` (excluding mongoc/ subdir)**

Run:

```bash
grep -rn "mongoc_\|bson_\|<mongoc.h>\|<bson.h>" src/ lib/dbi/*.c lib/dbi/*.h 2>/dev/null
```

Expected: zero matches. If any appear, find their cause:
- `lib/dbi/*.c` or `lib/dbi/*.h` matches mean a refactor was missed; address by relocating the code into `lib/dbi/mongoc/`.
- `src/` matches likely mean an `#include "ogs-mongoc.h"` that pulled in mongoc types. Trace and remove the include; replace any uses with the neutral API.

- [ ] **Step 2: Confirm the HSS no longer includes mongoc headers**

Run:

```bash
grep -rn "mongoc\|bson" src/hss/
```

Expected: no matches. If there are matches, they must be removed.

- [ ] **Step 3: Confirm the public dbi headers don't leak mongoc types**

Run:

```bash
grep -rn "mongoc\|bson_t\|BSON" lib/dbi/ogs-dbi.h lib/dbi/ogs-dbi-backend.h
```

Expected: no matches.

- [ ] **Step 4: Commit a verification note (no code changes)**

```bash
git commit --allow-empty -m "dbi: verify backend leak-tightness

No mongoc/bson symbols remain outside lib/dbi/mongoc/. The public
dbi API is fully backend-agnostic."
```

---

## Task 15: Build matrix verification — mongo-only and (placeholder) mongo-disabled

**Files:**
- Read-only verification + small adjustments if needed

- [ ] **Step 1: Mongo-enabled build (default)**

```bash
rm -rf build && meson setup build -Dmongo=enabled -Dredis=disabled
meson compile -C build
meson test -C build --suite unit --suite dbi -v
```

Expected: clean build, all dbi + unit tests pass.

- [ ] **Step 2: Mongo-disabled build (no Redis backend yet — must error out cleanly)**

```bash
rm -rf build && meson setup build -Dmongo=disabled -Dredis=disabled
```

Expected: meson **errors at configure time** with `At least one of -Dmongo / -Dredis must be enabled`. This is the intended behavior per the spec.

- [ ] **Step 3: Auto-mode build (the default user experience)**

```bash
rm -rf build && meson setup build
meson compile -C build
```

Expected: detects libmongoc, builds with `-DOGS_DBI_HAVE_MONGOC`. The redis dep is "auto" so it tries to find hiredis; if absent, that's fine (Phase 2 adds the real Redis sources).

- [ ] **Step 4: Run the full test suite against a real MongoDB instance to confirm no regression**

Bring up a local MongoDB (for example via Docker) and run integration tests that exercise the dbi path. Example:

```bash
docker run -d --rm --name open5gs-mongo-phase1 \
    -p 27017:27017 mongo:7
# Wait a few seconds for mongo to listen.
meson test -C build -v
docker stop open5gs-mongo-phase1
```

Expected: all tests that previously passed still pass. If any test fails because of the dbi refactor (as opposed to needing fixtures), revert to the last known-good commit and re-do the smallest task that introduced the regression.

- [ ] **Step 5: Manual smoke test — bring up open5gs against MongoDB and observe attach**

This is documented as a soft pre-release check in the spec (section 8.9). It is recommended but not blocking on Phase 1 completion. Example:

```bash
# In one shell:
./build/src/mme/open5gs-mmed -c configs/sample.yaml
# In another:
./build/src/hss/open5gs-hssd -c configs/sample.yaml
# (Plus SGW-C, SGW-U, PCRF, etc., as needed for a 4G attach.)
```

Use whatever UE simulator the project uses (`./build/tests/registration/...` or similar) and observe a successful attach. Logs should show `MongoDB URI: mongodb://localhost/open5gs` from `ogs_mongoc_init` and no errors from the dbi layer.

- [ ] **Step 6: Commit a tag for Phase 1 completion**

```bash
git tag -a phase-1-complete -m "Phase 1: dbi backend abstraction complete

- Vtable-based backend registry in lib/dbi/
- MongoDB code relocated into lib/dbi/mongoc/
- Backend-neutral ogs_dbi_change_event_t consumed by HSS
- Meson options -Dmongo / -Dredis (redis is a Phase 2 placeholder)
- No public API change; existing MongoDB deployments unaffected"
```

(Do not push the tag without the user's go-ahead; tags on a remote are visible to other contributors.)

---

## Self-Review

Verifying coverage against the spec:

**Spec section 3.1 (vtable):** Tasks 4 (declare) + 5 (implement & test) + 6 (register from mongoc). ✓

**Spec section 3.2 (file layout):** Tasks 6 (mongoc-internal.h, mongoc-backend.c) + 7–11 (per-source moves). ✓

**Spec section 3.3 (backwards compatibility):** Task 6 Step 4 keeps `ogs-mongoc.h` as a public compat shim. Task 10 keeps the public `ogs_dbi_*` signatures unchanged. ✓

**Spec section 5.0 (neutral change-event payload + HSS edits):** Tasks 2 (declare types), 3 (impl + test alloc/free), 12 (BSON-to-neutral builder), 13 (HSS signature change and event-bus rename). ✓

**Spec section 5.1 (BSON walker moves from hss-context.c to mongoc-watch.c):** Task 12. ✓

**Spec section 7 (meson options):** Task 1 (add options), Task 6 Step 5 (gate sources). ✓

**Phase 1 risk #4 (HSS signature change — must compile clean):** Task 13 plus Task 14's verification grep. ✓

**Phase 1 risk #5 (field-mask drift):** The comment block in Task 2 Step 2 and the BSON-walker comment in Task 12 Step 1 document the field-name → mask invariant. ✓

**Phase 2–5 scope:** Each gets its own plan after Phase 1 lands. Noted in the plan header.

**Placeholder scan:** No "TBD", no "TODO" left in the body. The Task 13 Step 3 instruction "Copy the original tail of `hss_handle_change_event` verbatim" is the only place the engineer must read existing code rather than copy-paste from this plan — that's intentional because the deleted body is long (~80 lines) and including it verbatim would balloon the plan with code the engineer can `git show` for themselves. The `git show HEAD:src/hss/hss-context.c | sed -n '1432,1540p'` command in Step 3 makes the read mechanical.

**Type consistency:** `ogs_dbi_field_e` (enum tag) vs `ogs_dbi_field_t` (no typedef — fields just use `uint32_t`). The struct field is `updated_fields_mask` of type `uint32_t`; the enum bits are constants. The spec used `ogs_dbi_field_t` informally; the plan uses `ogs_dbi_field_e` for the tag and `uint32_t` for the actual storage type. Consistent in all tasks.

Function names referenced across tasks:
- `ogs_dbi_change_event_alloc / _free` — declared Task 2, defined Task 3, used Task 13. ✓
- `ogs_dbi_backend_register / _find / current_backend` — declared Task 4, defined Task 5, used Task 6 + everywhere downstream. ✓
- `mongoc_build_change_event` — defined Task 12, wired in Task 13 Step 6. ✓
- `hss_dbi_event_push` — introduced (as `_push_bson`) Task 11; renamed and retyped Task 13. ✓
- `mongoc_poll_change_stream` — stubbed Task 6, implemented Task 11, rewired Task 13. ✓

The plan compiles cleanly in the engineer's head.
