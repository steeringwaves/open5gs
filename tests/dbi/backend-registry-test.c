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
    int rv = ogs_dbi_backend_register(&stub_backend);
    ABTS_INT_EQUAL(tc, OGS_ERROR, rv);
}

static void registry_find_unknown_returns_null(abts_case *tc, void *data)
{
    ABTS_PTR_EQUAL(tc, NULL, (void *)ogs_dbi_backend_find("does-not-exist"));
}

static void init_dispatches_by_scheme(abts_case *tc, void *data)
{
    reset_stub();

    int rv = ogs_dbi_init("stub://host:1234/dbname");
    ABTS_INT_EQUAL(tc, OGS_OK, rv);
    ABTS_INT_EQUAL(tc, 1, stub_init_called);
    ABTS_STR_EQUAL(tc, "stub://host:1234/dbname", stub_last_uri);
    ABTS_PTR_EQUAL(tc, (void *)&stub_backend, (void *)ogs_dbi_current_backend());

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
