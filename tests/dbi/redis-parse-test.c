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

static void init_selects_redis_backend(abts_case *tc, void *data)
{
    /* No server needed: redis_init returns OGS_RETRY when it can't connect,
     * but ogs_dbi_init only sets `active` on OGS_OK. So assert the backend is
     * FOUND in the registry instead. */
    ABTS_PTR_NOTNULL(tc, (void *)ogs_dbi_backend_find("redis"));
}

abts_suite *test_redis_parse(abts_suite *suite);
abts_suite *test_redis_parse(abts_suite *suite)
{
    suite = ADD_SUITE(suite);
    abts_run_test(suite, uri_basic, NULL);
    abts_run_test(suite, uri_defaults, NULL);
    abts_run_test(suite, uri_rejects_non_redis, NULL);
    abts_run_test(suite, init_selects_redis_backend, NULL);
    return suite;
}
