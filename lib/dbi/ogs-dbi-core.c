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
    ogs_free(e->imsi_bcd);
    ogs_free(e);
}
