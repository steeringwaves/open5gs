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
