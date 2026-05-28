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
    /* reaching here means free(NULL) did not crash */
    ABTS_TRUE(tc, 1);
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
