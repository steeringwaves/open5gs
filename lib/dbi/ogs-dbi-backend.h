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
 * INTERNAL header. Only included by lib/dbi/*.c and lib/dbi/<backend>/*.c.
 * Not installed.
 */

#if !defined(OGS_DBI_INSIDE) && !defined(OGS_DBI_COMPILATION)
#error "This header cannot be included directly."
#endif

#ifndef OGS_DBI_BACKEND_H
#define OGS_DBI_BACKEND_H

#include "ogs-dbi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ogs_dbi_backend_s {
    const char *name;       /* "mongoc", "redis" */
    const char *scheme;     /* "mongodb", "redis" */

    int  (*init)(const char *uri);
    void (*final)(void);

    int  (*auth_info)(char *supi, ogs_dbi_auth_info_t *out);
    int  (*update_sqn)(char *supi, uint64_t sqn);
    int  (*increment_sqn)(char *supi);
    int  (*update_imeisv)(char *supi, char *imeisv);
    int  (*update_mme)(char *supi, char *host, char *realm, bool purge);

    int  (*subscription_data)(char *supi, ogs_subscription_data_t *out);
    int  (*session_data)(const char *supi, const ogs_s_nssai_t *s_nssai,
                         const char *dnn, ogs_session_data_t *out);
    int  (*msisdn_data)(char *id, ogs_msisdn_data_t *out);
    int  (*ims_data)(char *supi, ogs_ims_data_t *out);

    /* Optional. NULL means "this backend does not support change events." */
    int  (*watch_init)(void);
    int  (*poll_change_stream)(void);
} ogs_dbi_backend_t;

/*
 * Backend registration. Called from each backend's translation unit at
 * library-load time. Returns OGS_OK on success, OGS_ERROR if a backend
 * with the same scheme is already registered.
 */
int ogs_dbi_backend_register(const ogs_dbi_backend_t *backend);

/* Lookup by URI scheme ("mongodb", "redis"). Returns NULL if not found. */
const ogs_dbi_backend_t *ogs_dbi_backend_find(const char *scheme);

/* The currently active backend, set by ogs_dbi_init(). NULL before init. */
const ogs_dbi_backend_t *ogs_dbi_current_backend(void);

#ifdef __cplusplus
}
#endif

#endif /* OGS_DBI_BACKEND_H */
