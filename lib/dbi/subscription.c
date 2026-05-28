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
