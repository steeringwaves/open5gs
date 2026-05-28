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

#include "mongoc-internal.h"

static ogs_mongoc_t self;

const ogs_dbi_backend_t mongoc_backend = {
    .name              = "mongoc",
    .scheme            = "mongodb",
    .init              = ogs_mongoc_init,
    .final             = ogs_mongoc_final,

    .auth_info         = mongoc_auth_info,
    .update_sqn        = mongoc_update_sqn,
    .increment_sqn     = mongoc_increment_sqn,
    .update_imeisv     = mongoc_update_imeisv,
    .update_mme        = mongoc_update_mme,

    .subscription_data = mongoc_subscription_data,
    .session_data      = mongoc_session_data,
    .msisdn_data       = mongoc_msisdn_data,
    .ims_data          = mongoc_ims_data,

    .watch_init         = mongoc_watch_init,
    .poll_change_stream = mongoc_poll_change_stream,
};

static bool
mongoc_get_server_status(mongoc_client_t *client,
                         mongoc_read_prefs_t *read_prefs,
                         bson_t *reply, bson_error_t *error)
{
    bson_t cmd = BSON_INITIALIZER;
    bool ret;

    BSON_ASSERT(client);
    BSON_APPEND_INT32(&cmd, "ping", 1);
    ret = mongoc_client_command_simple(
            client, "admin", &cmd, read_prefs, reply, error);
    bson_destroy(&cmd);
    return ret;
}

static char *masked_db_uri(const char *db_uri)
{
    char *tmp, *masked = NULL, *saveptr = NULL;
    char *array[2];

    ogs_assert(db_uri);
    tmp = ogs_strdup(db_uri);
    ogs_assert(tmp);

    memset(array, 0, sizeof(array));
    array[0] = ogs_strtok_r(tmp, "@", &saveptr);
    if (array[0])
        array[1] = ogs_strtok_r(NULL, "@", &saveptr);

    if (array[1]) {
        masked = ogs_msprintf("mongodb://*****:*****@%s", array[1]);
        ogs_assert(masked);
    } else {
        masked = ogs_strdup(array[0]);
        ogs_assert(masked);
    }
    ogs_free(tmp);
    return masked;
}

int ogs_mongoc_init(const char *db_uri)
{
    bson_t reply;
    bson_error_t error;
    bson_iter_t iter;
    const mongoc_uri_t *uri;

    if (!db_uri) {
        ogs_error("No DB_URI");
        return OGS_ERROR;
    }

    memset(&self, 0, sizeof(ogs_mongoc_t));

    self.masked_db_uri = masked_db_uri(db_uri);

    mongoc_init();
    self.initialized = true;

    self.client = mongoc_client_new(db_uri);
    if (!self.client) {
        ogs_error("Failed to parse DB URI [%s]", self.masked_db_uri);
        return OGS_ERROR;
    }

#if MONGOC_CHECK_VERSION(1, 4, 0)
    mongoc_client_set_error_api(self.client, 2);
#endif

    uri = mongoc_client_get_uri(self.client);
    ogs_assert(uri);

    self.name = mongoc_uri_get_database(uri);
    ogs_assert(self.name);

    self.database = mongoc_client_get_database(self.client, self.name);
    ogs_assert(self.database);

    if (!mongoc_get_server_status(self.client, NULL, &reply, &error)) {
        ogs_warn("Failed to connect to server [%s]", self.masked_db_uri);
        return OGS_RETRY;
    }
    ogs_assert(bson_iter_init_find(&iter, &reply, "ok"));
    bson_destroy(&reply);

    ogs_info("MongoDB URI: '%s'", self.masked_db_uri);

    /* Open the subscriber collection. (This used to be done by the old
     * ogs_dbi_init(), which has been replaced by the vtable dispatcher.) */
    self.collection.subscriber = mongoc_client_get_collection(
            self.client, self.name, "subscribers");
    ogs_assert(self.collection.subscriber);

    return OGS_OK;
}

void ogs_mongoc_final(void)
{
    if (self.collection.subscriber) {
        mongoc_collection_destroy(self.collection.subscriber);
        self.collection.subscriber = NULL;
    }

#if MONGOC_CHECK_VERSION(1, 9, 0)
    if (self.stream) {
        mongoc_change_stream_destroy(self.stream);
        self.stream = NULL;
    }
#endif

    if (self.database) {
        mongoc_database_destroy(self.database);
        self.database = NULL;
    }
    if (self.client) {
        mongoc_client_destroy(self.client);
        self.client = NULL;
    }
    if (self.masked_db_uri) {
        ogs_free(self.masked_db_uri);
        self.masked_db_uri = NULL;
    }

    if (self.initialized) {
        mongoc_cleanup();
        self.initialized = false;
    }
}

ogs_mongoc_t *ogs_mongoc(void)
{
    return &self;
}

/* ---- TEMPORARY STUBS — replaced as later tasks move the real code in.
 * These should never be invoked during Phase 1 because the public
 * ogs_dbi_* functions still call the original implementations directly
 * until Task 10 converts them to dispatchers. ---- */
int mongoc_auth_info(char *supi, ogs_dbi_auth_info_t *out)
{ (void)supi; (void)out; ogs_fatal("mongoc_auth_info stub called"); return OGS_ERROR; }
int mongoc_update_sqn(char *supi, uint64_t sqn)
{ (void)supi; (void)sqn; ogs_fatal("mongoc_update_sqn stub called"); return OGS_ERROR; }
int mongoc_increment_sqn(char *supi)
{ (void)supi; ogs_fatal("mongoc_increment_sqn stub called"); return OGS_ERROR; }
int mongoc_update_imeisv(char *supi, char *imeisv)
{ (void)supi; (void)imeisv; ogs_fatal("mongoc_update_imeisv stub called"); return OGS_ERROR; }
int mongoc_update_mme(char *supi, char *host, char *realm, bool purge)
{ (void)supi; (void)host; (void)realm; (void)purge; ogs_fatal("mongoc_update_mme stub called"); return OGS_ERROR; }
int mongoc_subscription_data(char *supi, ogs_subscription_data_t *out)
{ (void)supi; (void)out; ogs_fatal("mongoc_subscription_data stub called"); return OGS_ERROR; }
int mongoc_session_data(const char *supi, const ogs_s_nssai_t *s_nssai,
        const char *dnn, ogs_session_data_t *out)
{ (void)supi; (void)s_nssai; (void)dnn; (void)out; ogs_fatal("mongoc_session_data stub called"); return OGS_ERROR; }
int mongoc_msisdn_data(char *id, ogs_msisdn_data_t *out)
{ (void)id; (void)out; ogs_fatal("mongoc_msisdn_data stub called"); return OGS_ERROR; }
int mongoc_ims_data(char *supi, ogs_ims_data_t *out)
{ (void)supi; (void)out; ogs_fatal("mongoc_ims_data stub called"); return OGS_ERROR; }
int mongoc_watch_init(void)
{ ogs_fatal("mongoc_watch_init stub called"); return OGS_ERROR; }
int mongoc_poll_change_stream(void)
{ ogs_fatal("mongoc_poll_change_stream stub called"); return OGS_ERROR; }
