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
#include "ogs-dbi-backend.h"

#ifdef OGS_DBI_HAVE_MONGOC
extern const ogs_dbi_backend_t mongoc_backend;
#endif

#ifdef OGS_DBI_HAVE_REDIS
extern const ogs_dbi_backend_t redis_backend;
#endif

int __ogs_dbi_domain;

#define OGS_DBI_MAX_BACKENDS 4

static struct {
    const ogs_dbi_backend_t *entries[OGS_DBI_MAX_BACKENDS];
    int count;
} backends;

static const ogs_dbi_backend_t *active;

static ogs_dbi_change_handler_f change_handler;
static void *change_handler_data;

int ogs_dbi_backend_register(const ogs_dbi_backend_t *backend)
{
    int i;

    ogs_assert(backend);
    ogs_assert(backend->scheme);

    for (i = 0; i < backends.count; i++) {
        if (!strcmp(backends.entries[i]->scheme, backend->scheme)) {
            ogs_warn("dbi backend [%s] already registered", backend->scheme);
            return OGS_ERROR;
        }
    }

    if (backends.count >= OGS_DBI_MAX_BACKENDS) {
        ogs_error("dbi backend table full");
        return OGS_ERROR;
    }

    backends.entries[backends.count++] = backend;
    return OGS_OK;
}

const ogs_dbi_backend_t *ogs_dbi_backend_find(const char *scheme)
{
    int i;

    if (!scheme)
        return NULL;

    for (i = 0; i < backends.count; i++) {
        if (!strcmp(backends.entries[i]->scheme, scheme))
            return backends.entries[i];
    }
    return NULL;
}

const ogs_dbi_backend_t *ogs_dbi_current_backend(void)
{
    return active;
}

void ogs_dbi_set_change_handler(ogs_dbi_change_handler_f handler, void *data)
{
    change_handler = handler;
    change_handler_data = data;
}

void ogs_dbi_dispatch_change_event(ogs_dbi_change_event_t *event)
{
    if (!event)
        return;

    if (change_handler) {
        change_handler(event, change_handler_data);
    } else {
        ogs_dbi_change_event_free(event);
    }
}

static char *extract_scheme(const char *uri)
{
    const char *sep;
    size_t len;
    char *out;

    if (!uri)
        return NULL;

    sep = strstr(uri, "://");
    if (!sep)
        return NULL;

    len = sep - uri;
    out = ogs_malloc(len + 1);
    ogs_assert(out);
    memcpy(out, uri, len);
    out[len] = '\0';
    return out;
}

static void register_builtin_backends(void)
{
    static bool registered = false;

    if (registered)
        return;
    registered = true;

#ifdef OGS_DBI_HAVE_MONGOC
    ogs_dbi_backend_register(&mongoc_backend);
#endif

#ifdef OGS_DBI_HAVE_REDIS
    ogs_dbi_backend_register(&redis_backend);
#endif
}

int ogs_dbi_init(const char *uri)
{
    char *scheme;
    const ogs_dbi_backend_t *backend;
    int rv;

    if (!uri) {
        ogs_error("dbi init: NULL URI");
        return OGS_ERROR;
    }

    register_builtin_backends();

    scheme = extract_scheme(uri);
    if (!scheme) {
        ogs_error("dbi init: malformed URI '%s' (missing scheme://)", uri);
        return OGS_ERROR;
    }

    backend = ogs_dbi_backend_find(scheme);
    if (!backend) {
        ogs_error("dbi init: no backend registered for scheme '%s'", scheme);
        ogs_free(scheme);
        return OGS_ERROR;
    }
    ogs_free(scheme);

    rv = backend->init(uri);
    if (rv != OGS_OK)
        return rv;

    active = backend;
    return OGS_OK;
}

void ogs_dbi_final(void)
{
    if (active && active->final)
        active->final();
    active = NULL;
}

int ogs_dbi_collection_watch_init(void)
{
    if (!active || !active->watch_init) {
        ogs_warn("dbi: active backend does not support change watching");
        return OGS_ERROR;
    }
    return active->watch_init();
}

int ogs_dbi_poll_change_stream(void)
{
    if (!active || !active->poll_change_stream)
        return OGS_ERROR;
    return active->poll_change_stream();
}

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
