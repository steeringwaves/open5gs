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
