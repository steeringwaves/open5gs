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

/*
 * Internal mongoc-backend declarations. Included only by the mongoc
 * backend C files under lib/dbi/mongoc, never by callers.
 */

#ifndef OGS_DBI_MONGOC_INTERNAL_H
#define OGS_DBI_MONGOC_INTERNAL_H

#include "ogs-dbi-backend.h"   /* pulls ogs-dbi.h, which pulls ogs-mongoc.h (ogs_mongoc_t) */
#include "ogs-mongoc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The vtable instance, defined in mongoc-backend.c. */
extern const ogs_dbi_backend_t mongoc_backend;

/* Backend method implementations. Naming: drop `ogs_dbi_`, prepend `mongoc_`.
 * Defined across mongoc-{subscription,session,ims,watch}.c. */
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
