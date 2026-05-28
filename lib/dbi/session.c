/*
 * Copyright (C) 2019-2026 by Sukchan Lee <acetcom@gmail.com>
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
#include "ogs-dbi-backend.h"

int ogs_dbi_session_data(
        const char *supi, const ogs_s_nssai_t *s_nssai, const char *dnn,
        ogs_session_data_t *session_data)
{
    const ogs_dbi_backend_t *b = ogs_dbi_current_backend();
    ogs_assert(b);
    return b->session_data(supi, s_nssai, dnn, session_data);
}
