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
