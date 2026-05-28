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

#include "ogs-core.h"
#include "dbctl-redis.h"
#include "dbctl-subscriber.h"

#include <getopt.h>

static int __dbctl_log_domain;
#undef OGS_LOG_DOMAIN
#define OGS_LOG_DOMAIN __dbctl_log_domain

#define DBCTL_PROG "open5gs-dbctl-redis"

static void usage(FILE *out)
{
    fprintf(out,
"Usage: %s --db-uri redis://host:port/?prefix=open5gs: <cmd> [args]\n"
"\n"
"Redis-backed subscriber provisioning for Open5GS.\n"
"\n"
"Options:\n"
"  --db-uri <uri>    Redis URI (default: $DB_URI). e.g.\n"
"                    redis://127.0.0.1:6379/?prefix=open5gs:\n"
"  -h, --help        Show this help and exit.\n"
"\n"
"Commands:\n"
"  add   --imsi <imsi> --key <K> (--opc <OPC> | --op <OP>) [--amf <amf>]\n"
"        [--apn <dnn>] [--sst <sst>] [--sd <sd>] [--msisdn <bcd>]\n"
"  del   --imsi <imsi>\n"
"  show  --imsi <imsi>\n"
"  list  [--limit <n>]\n"
"  import --file <path>            # mongoexport JSON array or JSONL\n"
"  export --file <path>            # JSONL\n"
"  msisdn-add --imsi <imsi> --msisdn <bcd>\n"
"  msisdn-del --msisdn <bcd>\n"
"  reset-sqn --imsi <imsi> [--value <n>]\n"
"\n"
"The --db-uri value may also be supplied via the DB_URI environment variable.\n",
        DBCTL_PROG);
}

/*
 * Hidden connectivity check: open the connection and issue a PING.
 * Returns OGS_OK on PONG, OGS_ERROR otherwise.
 */
static int cmd_ping(dbctl_redis_t *db)
{
    redisReply *reply;
    int rv = OGS_ERROR;

    reply = redisCommand(db->ctx, "PING");
    if (reply == NULL) {
        ogs_error("PING failed: %s", db->ctx->errstr);
        return OGS_ERROR;
    }

    if (reply->type == REDIS_REPLY_STATUS ||
            reply->type == REDIS_REPLY_STRING) {
        printf("%s\n", reply->str);
        rv = OGS_OK;
    } else if (reply->type == REDIS_REPLY_ERROR) {
        ogs_error("PING error: %s", reply->str);
        rv = OGS_ERROR;
    } else {
        ogs_error("PING returned an unexpected reply type %d", reply->type);
        rv = OGS_ERROR;
    }

    freeReplyObject(reply);
    return rv;
}

/*
 * Per-command option set. getopt_long in the command handlers starts from
 * argv[optind] (the subcommand) so the subcommand itself becomes argv[0] of
 * the sub-parse and is skipped.
 */
enum {
    OPT_IMSI = 256, OPT_KEY, OPT_OPC, OPT_OP, OPT_AMF, OPT_RAND,
    OPT_APN, OPT_SST, OPT_SD, OPT_MSISDN, OPT_LIMIT, OPT_VALUE, OPT_FILE
};

static const struct option cmd_long_options[] = {
    { "imsi",   required_argument, NULL, OPT_IMSI },
    { "key",    required_argument, NULL, OPT_KEY },
    { "opc",    required_argument, NULL, OPT_OPC },
    { "op",     required_argument, NULL, OPT_OP },
    { "amf",    required_argument, NULL, OPT_AMF },
    { "rand",   required_argument, NULL, OPT_RAND },
    { "apn",    required_argument, NULL, OPT_APN },
    { "sst",    required_argument, NULL, OPT_SST },
    { "sd",     required_argument, NULL, OPT_SD },
    { "msisdn", required_argument, NULL, OPT_MSISDN },
    { "limit",  required_argument, NULL, OPT_LIMIT },
    { "value",  required_argument, NULL, OPT_VALUE },
    { "file",   required_argument, NULL, OPT_FILE },
    { NULL,     0,                 NULL, 0 }
};

static int cmd_add(dbctl_redis_t *db, int argc, char *argv[])
{
    dbctl_add_args_t args;
    cJSON *doc;
    int c, have_opc = 0, have_op = 0;
    int rv = OGS_ERROR;

    memset(&args, 0, sizeof(args));

    optind = 1;
    opterr = 0;
    while ((c = getopt_long(argc, argv, "", cmd_long_options, NULL)) != -1) {
        switch (c) {
        case OPT_IMSI:   args.imsi = optarg; break;
        case OPT_KEY:    args.k = optarg; break;
        case OPT_OPC:    args.opc = optarg; have_opc = 1; break;
        case OPT_OP:     args.op = optarg; have_op = 1; break;
        case OPT_AMF:    args.amf = optarg; break;
        case OPT_RAND:   args.rand = optarg; break;
        case OPT_APN:    args.apn = optarg; break;
        case OPT_SST:    args.sst = atoi(optarg); break;
        case OPT_SD:     args.sd = optarg; args.has_sd = 1; break;
        case OPT_MSISDN: args.msisdn = optarg; break;
        default:
            ogs_error("add: unknown or invalid option");
            return OGS_ERROR;
        }
    }

    if (!args.imsi) {
        ogs_error("add: --imsi is required");
        return OGS_ERROR;
    }
    if (!args.k) {
        ogs_error("add: --key is required");
        return OGS_ERROR;
    }
    if (have_opc && have_op) {
        ogs_error("add: use only one of --opc / --op");
        return OGS_ERROR;
    }
    if (!have_opc && !have_op) {
        ogs_error("add: one of --opc / --op is required");
        return OGS_ERROR;
    }
    args.use_opc = have_opc;

    doc = dbctl_build_subscriber(&args);
    if (!doc) {
        ogs_error("add: failed to build subscriber document");
        return OGS_ERROR;
    }

    if (dbctl_redis_set_subscriber(db, args.imsi, doc) != OGS_OK) {
        ogs_error("add: failed to store subscriber [%s]", args.imsi);
        goto cleanup;
    }

    if (args.msisdn) {
        if (dbctl_redis_set_msisdn_index(db, args.msisdn, args.imsi)
                != OGS_OK) {
            ogs_error("add: failed to index msisdn [%s]", args.msisdn);
            goto cleanup;
        }
    }

    printf("Added subscriber %s\n", args.imsi);
    rv = OGS_OK;

cleanup:
    cJSON_Delete(doc);
    return rv;
}

static int cmd_show(dbctl_redis_t *db, int argc, char *argv[])
{
    const char *imsi = NULL;
    char *json = NULL, *pretty = NULL;
    cJSON *doc = NULL;
    int c, rv = OGS_ERROR;

    optind = 1;
    opterr = 0;
    while ((c = getopt_long(argc, argv, "", cmd_long_options, NULL)) != -1) {
        switch (c) {
        case OPT_IMSI: imsi = optarg; break;
        default:
            ogs_error("show: unknown or invalid option");
            return OGS_ERROR;
        }
    }

    if (!imsi) {
        ogs_error("show: --imsi is required");
        return OGS_ERROR;
    }

    json = dbctl_redis_get(db, "subscriber", imsi);
    if (!json) {
        ogs_error("show: no such subscriber [%s]", imsi);
        return OGS_ERROR;
    }

    doc = cJSON_Parse(json);
    if (!doc) {
        ogs_error("show: malformed subscriber JSON [%s]", imsi);
        goto cleanup;
    }

    pretty = cJSON_Print(doc);
    if (!pretty) {
        ogs_error("show: failed to format subscriber [%s]", imsi);
        goto cleanup;
    }

    printf("%s\n", pretty);
    rv = OGS_OK;

cleanup:
    if (pretty) cJSON_free(pretty);
    if (doc) cJSON_Delete(doc);
    if (json) ogs_free(json);
    return rv;
}

static void list_print_imsi(const char *imsi, void *data)
{
    (void)data;
    printf("%s\n", imsi);
}

static int cmd_list(dbctl_redis_t *db, int argc, char *argv[])
{
    int limit = 0;
    int c, n;

    optind = 1;
    opterr = 0;
    while ((c = getopt_long(argc, argv, "", cmd_long_options, NULL)) != -1) {
        switch (c) {
        case OPT_LIMIT: limit = atoi(optarg); break;
        default:
            ogs_error("list: unknown or invalid option");
            return OGS_ERROR;
        }
    }

    n = dbctl_redis_scan_imsis(db, list_print_imsi, NULL, limit);
    if (n < 0) {
        ogs_error("list: SCAN failed");
        return OGS_ERROR;
    }

    return OGS_OK;
}

/*
 * GET <prefix>subscriber:<imsi> and parse it. Returns the parsed cJSON doc
 * (caller frees with cJSON_Delete) or NULL if the subscriber is absent or the
 * stored JSON is malformed. *existed (if non-NULL) distinguishes "absent" from
 * "present but malformed".
 */
static cJSON *get_subscriber_doc(dbctl_redis_t *db, const char *imsi,
        int *existed)
{
    char *json;
    cJSON *doc;

    if (existed) *existed = 0;

    json = dbctl_redis_get(db, "subscriber", imsi);
    if (!json)
        return NULL;
    if (existed) *existed = 1;

    doc = cJSON_Parse(json);
    ogs_free(json);
    if (!doc)
        ogs_error("malformed subscriber JSON [%s]", imsi);
    return doc;
}

static int cmd_del(dbctl_redis_t *db, int argc, char *argv[])
{
    const char *imsi = NULL;
    cJSON *doc = NULL, *msisdn_arr, *m;
    int c, existed = 0, rv = OGS_ERROR;

    optind = 1;
    opterr = 0;
    while ((c = getopt_long(argc, argv, "", cmd_long_options, NULL)) != -1) {
        switch (c) {
        case OPT_IMSI: imsi = optarg; break;
        default:
            ogs_error("del: unknown or invalid option");
            return OGS_ERROR;
        }
    }

    if (!imsi) {
        ogs_error("del: --imsi is required");
        return OGS_ERROR;
    }

    doc = get_subscriber_doc(db, imsi, &existed);
    if (!existed) {
        ogs_error("del: no such subscriber [%s]", imsi);
        return OGS_ERROR;
    }
    /* doc may be NULL if the stored JSON was malformed; still delete the key
     * but we cannot enumerate its msisdn[] index entries. */

    if (dbctl_redis_del(db, "subscriber", imsi) != OGS_OK) {
        ogs_error("del: failed to delete subscriber [%s]", imsi);
        goto cleanup;
    }

    /* Remove each secondary msisdn index entry the doc referenced. */
    if (doc) {
        msisdn_arr = cJSON_GetObjectItemCaseSensitive(doc, "msisdn");
        if (msisdn_arr && cJSON_IsArray(msisdn_arr)) {
            cJSON_ArrayForEach(m, msisdn_arr) {
                if (cJSON_IsString(m) && m->valuestring && m->valuestring[0]) {
                    if (dbctl_redis_del(db, "msisdn", m->valuestring) != OGS_OK)
                        ogs_warn("del: failed to delete msisdn index [%s]",
                                m->valuestring);
                }
            }
        }
    }

    /* Notify a running NF (refresh ALL fields -> it will see the deletion). */
    if (dbctl_redis_publish_change(db, imsi, NULL, 0) != OGS_OK)
        ogs_warn("del: failed to publish change event for [%s]", imsi);

    printf("Deleted subscriber %s\n", imsi);
    rv = OGS_OK;

cleanup:
    if (doc) cJSON_Delete(doc);
    return rv;
}

static int cmd_msisdn_add(dbctl_redis_t *db, int argc, char *argv[])
{
    const char *imsi = NULL, *msisdn = NULL;
    cJSON *doc = NULL, *arr, *m;
    int c, existed = 0, present = 0, rv = OGS_ERROR;

    optind = 1;
    opterr = 0;
    while ((c = getopt_long(argc, argv, "", cmd_long_options, NULL)) != -1) {
        switch (c) {
        case OPT_IMSI:   imsi = optarg; break;
        case OPT_MSISDN: msisdn = optarg; break;
        default:
            ogs_error("msisdn-add: unknown or invalid option");
            return OGS_ERROR;
        }
    }

    if (!imsi) {
        ogs_error("msisdn-add: --imsi is required");
        return OGS_ERROR;
    }
    if (!msisdn) {
        ogs_error("msisdn-add: --msisdn is required");
        return OGS_ERROR;
    }

    doc = get_subscriber_doc(db, imsi, &existed);
    if (!existed) {
        ogs_error("msisdn-add: no such subscriber [%s]", imsi);
        return OGS_ERROR;
    }
    if (!doc) {
        ogs_error("msisdn-add: malformed subscriber [%s]", imsi);
        return OGS_ERROR;
    }

    /* 1) maintain the secondary index <prefix>msisdn:<bcd> -> imsi */
    if (dbctl_redis_set_msisdn_index(db, msisdn, imsi) != OGS_OK) {
        ogs_error("msisdn-add: failed to index msisdn [%s]", msisdn);
        goto cleanup;
    }

    /* 2) add the msisdn into the subscriber doc's msisdn[] if not present */
    arr = cJSON_GetObjectItemCaseSensitive(doc, "msisdn");
    if (arr && !cJSON_IsArray(arr)) {
        /* Replace a non-array "msisdn" with a fresh array. */
        cJSON_DeleteItemFromObjectCaseSensitive(doc, "msisdn");
        arr = NULL;
    }
    if (!arr) {
        arr = cJSON_AddArrayToObject(doc, "msisdn");
        if (!arr) {
            ogs_error("msisdn-add: out of memory");
            goto cleanup;
        }
    }
    cJSON_ArrayForEach(m, arr) {
        if (cJSON_IsString(m) && m->valuestring &&
                !strcmp(m->valuestring, msisdn)) {
            present = 1;
            break;
        }
    }
    if (!present) {
        cJSON *s = cJSON_CreateString(msisdn);
        if (!s) {
            ogs_error("msisdn-add: out of memory");
            goto cleanup;
        }
        cJSON_AddItemToArray(arr, s);
        if (dbctl_redis_set_subscriber(db, imsi, doc) != OGS_OK) {
            ogs_error("msisdn-add: failed to update subscriber [%s]", imsi);
            goto cleanup;
        }
    }

    /*
     * "msisdn" is not an S6a field the watcher maps, so refresh ALL fields
     * (fields == NULL) rather than send an empty mask.
     */
    if (dbctl_redis_publish_change(db, imsi, NULL, 0) != OGS_OK)
        ogs_warn("msisdn-add: failed to publish change event for [%s]", imsi);

    printf("Added msisdn %s to %s\n", msisdn, imsi);
    rv = OGS_OK;

cleanup:
    if (doc) cJSON_Delete(doc);
    return rv;
}

static int cmd_msisdn_del(dbctl_redis_t *db, int argc, char *argv[])
{
    const char *msisdn = NULL;
    char *imsi = NULL;
    cJSON *doc = NULL, *arr, *m;
    int c, existed = 0, idx, rv = OGS_ERROR;

    optind = 1;
    opterr = 0;
    while ((c = getopt_long(argc, argv, "", cmd_long_options, NULL)) != -1) {
        switch (c) {
        case OPT_MSISDN: msisdn = optarg; break;
        default:
            ogs_error("msisdn-del: unknown or invalid option");
            return OGS_ERROR;
        }
    }

    if (!msisdn) {
        ogs_error("msisdn-del: --msisdn is required");
        return OGS_ERROR;
    }

    /* Resolve imsi from the secondary index; error if it does not exist. */
    imsi = dbctl_redis_get(db, "msisdn", msisdn);
    if (!imsi) {
        ogs_error("msisdn-del: no such msisdn index entry [%s]", msisdn);
        return OGS_ERROR;
    }

    if (dbctl_redis_del(db, "msisdn", msisdn) != OGS_OK) {
        ogs_error("msisdn-del: failed to delete msisdn index [%s]", msisdn);
        goto cleanup;
    }

    /* Remove the bcd from the subscriber doc's msisdn[] to stay consistent. */
    doc = get_subscriber_doc(db, imsi, &existed);
    if (doc) {
        arr = cJSON_GetObjectItemCaseSensitive(doc, "msisdn");
        if (arr && cJSON_IsArray(arr)) {
            idx = 0;
            cJSON_ArrayForEach(m, arr) {
                if (cJSON_IsString(m) && m->valuestring &&
                        !strcmp(m->valuestring, msisdn)) {
                    cJSON_DeleteItemFromArray(arr, idx);
                    break;
                }
                idx++;
            }
            if (dbctl_redis_set_subscriber(db, imsi, doc) != OGS_OK)
                ogs_warn("msisdn-del: failed to update subscriber [%s]", imsi);
        }
    } else if (existed) {
        ogs_warn("msisdn-del: malformed subscriber [%s], index removed only",
                imsi);
    }

    if (dbctl_redis_publish_change(db, imsi, NULL, 0) != OGS_OK)
        ogs_warn("msisdn-del: failed to publish change event for [%s]", imsi);

    printf("Removed msisdn %s from %s\n", msisdn, imsi);
    rv = OGS_OK;

cleanup:
    if (doc) cJSON_Delete(doc);
    if (imsi) ogs_free(imsi);
    return rv;
}

static int cmd_reset_sqn(dbctl_redis_t *db, int argc, char *argv[])
{
    const char *imsi = NULL;
    long value = 0;
    cJSON *doc = NULL, *security, *sqn;
    int c, existed = 0, rv = OGS_ERROR;

    optind = 1;
    opterr = 0;
    while ((c = getopt_long(argc, argv, "", cmd_long_options, NULL)) != -1) {
        switch (c) {
        case OPT_IMSI:  imsi = optarg; break;
        case OPT_VALUE: value = atol(optarg); break;
        default:
            ogs_error("reset-sqn: unknown or invalid option");
            return OGS_ERROR;
        }
    }

    if (!imsi) {
        ogs_error("reset-sqn: --imsi is required");
        return OGS_ERROR;
    }

    doc = get_subscriber_doc(db, imsi, &existed);
    if (!existed) {
        ogs_error("reset-sqn: no such subscriber [%s]", imsi);
        return OGS_ERROR;
    }
    if (!doc) {
        ogs_error("reset-sqn: malformed subscriber [%s]", imsi);
        return OGS_ERROR;
    }

    /* Ensure a security{} object exists. */
    security = cJSON_GetObjectItemCaseSensitive(doc, "security");
    if (security && !cJSON_IsObject(security)) {
        cJSON_DeleteItemFromObjectCaseSensitive(doc, "security");
        security = NULL;
    }
    if (!security) {
        security = cJSON_AddObjectToObject(doc, "security");
        if (!security) {
            ogs_error("reset-sqn: out of memory");
            goto cleanup;
        }
    }

    /* Replace security.sqn with the new numeric value (default 0). */
    sqn = cJSON_CreateNumber((double)value);
    if (!sqn) {
        ogs_error("reset-sqn: out of memory");
        goto cleanup;
    }
    if (cJSON_GetObjectItemCaseSensitive(security, "sqn"))
        cJSON_ReplaceItemInObjectCaseSensitive(security, "sqn", sqn);
    else
        cJSON_AddItemToObject(security, "sqn", sqn);

    if (dbctl_redis_set_subscriber(db, imsi, doc) != OGS_OK) {
        ogs_error("reset-sqn: failed to update subscriber [%s]", imsi);
        goto cleanup;
    }

    /* SQN is not S6a-relevant; deliberately do NOT publish (avoid noise). */

    printf("Reset sqn of %s to %ld\n", imsi, value);
    rv = OGS_OK;

cleanup:
    if (doc) cJSON_Delete(doc);
    return rv;
}

/*
 * Read the entire file at `path` into a NUL-terminated heap buffer. Returns the
 * buffer (caller frees with ogs_free) and, when `len_out` is non-NULL, the byte
 * length (excluding the terminator). Returns NULL on any error (logged).
 */
static char *read_whole_file(const char *path, size_t *len_out)
{
    FILE *fp;
    long size;
    size_t nread;
    char *buf;

    fp = fopen(path, "rb");
    if (!fp) {
        ogs_error("import: cannot open file [%s]: %s", path, strerror(errno));
        return NULL;
    }

    if (fseek(fp, 0, SEEK_END) != 0 || (size = ftell(fp)) < 0 ||
            fseek(fp, 0, SEEK_SET) != 0) {
        ogs_error("import: cannot determine size of [%s]", path);
        fclose(fp);
        return NULL;
    }

    buf = ogs_malloc((size_t)size + 1);
    if (!buf) {
        ogs_error("import: out of memory reading [%s]", path);
        fclose(fp);
        return NULL;
    }

    nread = fread(buf, 1, (size_t)size, fp);
    fclose(fp);

    if (nread != (size_t)size) {
        ogs_error("import: short read on [%s]", path);
        ogs_free(buf);
        return NULL;
    }

    buf[nread] = '\0';
    if (len_out) *len_out = nread;
    return buf;
}

/*
 * Store one parsed (and to-be-canonicalized) subscriber document. Extracts the
 * imsi, writes <prefix>subscriber:<imsi>, maintains the msisdn[] secondary
 * index, and publishes a change event. Returns OGS_OK on a stored subscriber,
 * OGS_RETRY when the doc has no imsi (skipped + warned), OGS_ERROR on failure.
 */
static int import_one(dbctl_redis_t *db, cJSON *doc)
{
    const char *imsi;
    cJSON *msisdn_arr, *m;

    dbctl_canonicalize_extended_json(doc);

    imsi = dbctl_subscriber_imsi(doc);
    if (!imsi || !imsi[0]) {
        ogs_warn("import: skipping a document with no imsi");
        return OGS_RETRY;
    }

    if (dbctl_redis_set_subscriber(db, imsi, doc) != OGS_OK) {
        ogs_error("import: failed to store subscriber [%s]", imsi);
        return OGS_ERROR;
    }

    msisdn_arr = cJSON_GetObjectItemCaseSensitive(doc, "msisdn");
    if (msisdn_arr && cJSON_IsArray(msisdn_arr)) {
        cJSON_ArrayForEach(m, msisdn_arr) {
            if (cJSON_IsString(m) && m->valuestring && m->valuestring[0]) {
                if (dbctl_redis_set_msisdn_index(db, m->valuestring, imsi)
                        != OGS_OK)
                    ogs_warn("import: failed to index msisdn [%s] for [%s]",
                            m->valuestring, imsi);
            }
        }
    }

    /* Notify a running NF (refresh ALL fields) for inserts and overwrites. */
    if (dbctl_redis_publish_change(db, imsi, NULL, 0) != OGS_OK)
        ogs_warn("import: failed to publish change event for [%s]", imsi);

    return OGS_OK;
}

static int cmd_import(dbctl_redis_t *db, int argc, char *argv[])
{
    const char *path = NULL;
    char *buf = NULL, *p;
    const char *first;
    int c, count = 0, rv = OGS_ERROR;

    optind = 1;
    opterr = 0;
    while ((c = getopt_long(argc, argv, "", cmd_long_options, NULL)) != -1) {
        switch (c) {
        case OPT_FILE: path = optarg; break;
        default:
            ogs_error("import: unknown or invalid option");
            return OGS_ERROR;
        }
    }

    if (!path) {
        ogs_error("import: --file is required");
        return OGS_ERROR;
    }

    buf = read_whole_file(path, NULL);
    if (!buf)
        return OGS_ERROR;

    /* Detect format: first non-whitespace '[' => JSON array, else JSONL. */
    first = buf;
    while (*first && isspace((unsigned char)*first))
        first++;

    if (*first == '[') {
        cJSON *array, *doc;

        array = cJSON_Parse(buf);
        if (!array || !cJSON_IsArray(array)) {
            ogs_error("import: file does not parse as a JSON array [%s]", path);
            if (array) cJSON_Delete(array);
            goto cleanup;
        }

        cJSON_ArrayForEach(doc, array) {
            int r = import_one(db, doc);
            if (r == OGS_OK)
                count++;
            else if (r == OGS_ERROR) {
                cJSON_Delete(array);
                goto cleanup;
            }
            /* OGS_RETRY: skipped (no imsi), already warned. */
        }
        cJSON_Delete(array);
    } else {
        /* JSONL: one document per non-empty line. */
        p = buf;
        while (*p) {
            char *nl = strchr(p, '\n');
            cJSON *doc;
            int r;

            if (nl) *nl = '\0';

            /* Skip blank / whitespace-only lines. */
            {
                char *s = p;
                while (*s && isspace((unsigned char)*s))
                    s++;
                if (*s == '\0') {
                    if (!nl) break;
                    p = nl + 1;
                    continue;
                }
            }

            doc = cJSON_Parse(p);
            if (!doc) {
                ogs_error("import: malformed JSON line in [%s]", path);
                goto cleanup;
            }

            r = import_one(db, doc);
            cJSON_Delete(doc);
            if (r == OGS_OK)
                count++;
            else if (r == OGS_ERROR)
                goto cleanup;
            /* OGS_RETRY: skipped (no imsi), already warned. */

            if (!nl) break;
            p = nl + 1;
        }
    }

    printf("imported %d subscribers\n", count);
    rv = OGS_OK;

cleanup:
    if (buf) ogs_free(buf);
    return rv;
}

/* Per-imsi export context: the file we write to and a running count. */
typedef struct export_ctx_s {
    dbctl_redis_t *db;
    FILE *fp;
    int count;
    int failed;
} export_ctx_t;

static void export_one(const char *imsi, void *data)
{
    export_ctx_t *ctx = data;
    char *json;

    if (ctx->failed)
        return;

    /* Write the RAW stored JSON verbatim as one JSONL line. */
    json = dbctl_redis_get(ctx->db, "subscriber", imsi);
    if (!json) {
        ogs_warn("export: subscriber [%s] vanished during export", imsi);
        return;
    }

    if (fprintf(ctx->fp, "%s\n", json) < 0) {
        ogs_error("export: write failed for [%s]", imsi);
        ctx->failed = 1;
    } else {
        ctx->count++;
    }

    ogs_free(json);
}

static int cmd_export(dbctl_redis_t *db, int argc, char *argv[])
{
    const char *path = NULL;
    export_ctx_t ctx;
    int c, n, rv = OGS_ERROR;

    optind = 1;
    opterr = 0;
    while ((c = getopt_long(argc, argv, "", cmd_long_options, NULL)) != -1) {
        switch (c) {
        case OPT_FILE: path = optarg; break;
        default:
            ogs_error("export: unknown or invalid option");
            return OGS_ERROR;
        }
    }

    if (!path) {
        ogs_error("export: --file is required");
        return OGS_ERROR;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.db = db;
    ctx.fp = fopen(path, "wb");
    if (!ctx.fp) {
        ogs_error("export: cannot open file [%s]: %s", path, strerror(errno));
        return OGS_ERROR;
    }

    n = dbctl_redis_scan_imsis(db, export_one, &ctx, 0);
    if (n < 0) {
        ogs_error("export: SCAN failed");
        goto cleanup;
    }
    if (ctx.failed)
        goto cleanup;

    printf("exported %d subscribers\n", ctx.count);
    rv = OGS_OK;

cleanup:
    if (ctx.fp) {
        if (fclose(ctx.fp) != 0 && rv == OGS_OK) {
            ogs_error("export: error closing [%s]", path);
            rv = OGS_ERROR;
        }
    }
    return rv;
}

int main(int argc, char *argv[])
{
    int rv = EXIT_FAILURE;
    int connected = 0;
    const char *db_uri = NULL;
    const char *cmd = NULL;
    dbctl_redis_t db;

    /* Index in argv of the first non-option argument (the subcommand). */
    int c;
    int help = 0;

    static struct option long_options[] = {
        { "db-uri", required_argument, NULL, 'u' },
        { "help",   no_argument,       NULL, 'h' },
        { NULL,     0,                 NULL,  0  }
    };

    ogs_core_initialize();
    ogs_log_install_domain(&__dbctl_log_domain, "dbctl", OGS_LOG_INFO);

    /*
     * Only parse the leading options that belong to the tool itself; stop at
     * the first non-option (the subcommand) so per-command flags are left for
     * the command handlers in later tasks. '+' keeps getopt from permuting.
     */
    opterr = 0;
    while ((c = getopt_long(argc, argv, "+hu:", long_options, NULL)) != -1) {
        switch (c) {
        case 'u':
            db_uri = optarg;
            break;
        case 'h':
            help = 1;
            break;
        case '?':
        default:
            ogs_error("Unknown or incomplete option near '%s'",
                    argv[optind ? optind - 1 : 0]);
            usage(stderr);
            goto cleanup;
        }
    }

    if (help) {
        usage(stdout);
        rv = EXIT_SUCCESS;
        goto cleanup;
    }

    if (optind >= argc) {
        ogs_error("No command given");
        usage(stderr);
        goto cleanup;
    }
    cmd = argv[optind];

    if (!db_uri) db_uri = getenv("DB_URI");
    if (!db_uri) {
        ogs_error("No Redis URI: pass --db-uri or set DB_URI");
        usage(stderr);
        goto cleanup;
    }

    /*
     * Every command needs a live connection. Open it once here; command
     * handlers receive the connected handle.
     */
    if (dbctl_redis_open(db_uri, &db) != OGS_OK) {
        /* dbctl_redis_open already logged the reason. */
        connected = 0;
        goto cleanup;
    }
    connected = 1;

    {
        /* The command handlers re-parse flags starting at the subcommand. */
        int cmd_argc = argc - optind;
        char **cmd_argv = &argv[optind];

        if (!strcmp(cmd, "ping")) {
            rv = (cmd_ping(&db) == OGS_OK) ? EXIT_SUCCESS : EXIT_FAILURE;
        } else if (!strcmp(cmd, "add")) {
            rv = (cmd_add(&db, cmd_argc, cmd_argv) == OGS_OK) ?
                    EXIT_SUCCESS : EXIT_FAILURE;
        } else if (!strcmp(cmd, "show")) {
            rv = (cmd_show(&db, cmd_argc, cmd_argv) == OGS_OK) ?
                    EXIT_SUCCESS : EXIT_FAILURE;
        } else if (!strcmp(cmd, "list")) {
            rv = (cmd_list(&db, cmd_argc, cmd_argv) == OGS_OK) ?
                    EXIT_SUCCESS : EXIT_FAILURE;
        } else if (!strcmp(cmd, "del")) {
            rv = (cmd_del(&db, cmd_argc, cmd_argv) == OGS_OK) ?
                    EXIT_SUCCESS : EXIT_FAILURE;
        } else if (!strcmp(cmd, "msisdn-add")) {
            rv = (cmd_msisdn_add(&db, cmd_argc, cmd_argv) == OGS_OK) ?
                    EXIT_SUCCESS : EXIT_FAILURE;
        } else if (!strcmp(cmd, "msisdn-del")) {
            rv = (cmd_msisdn_del(&db, cmd_argc, cmd_argv) == OGS_OK) ?
                    EXIT_SUCCESS : EXIT_FAILURE;
        } else if (!strcmp(cmd, "reset-sqn")) {
            rv = (cmd_reset_sqn(&db, cmd_argc, cmd_argv) == OGS_OK) ?
                    EXIT_SUCCESS : EXIT_FAILURE;
        } else if (!strcmp(cmd, "import")) {
            rv = (cmd_import(&db, cmd_argc, cmd_argv) == OGS_OK) ?
                    EXIT_SUCCESS : EXIT_FAILURE;
        } else if (!strcmp(cmd, "export")) {
            rv = (cmd_export(&db, cmd_argc, cmd_argv) == OGS_OK) ?
                    EXIT_SUCCESS : EXIT_FAILURE;
        } else {
            ogs_error("Unknown command: %s", cmd);
            usage(stderr);
            rv = EXIT_FAILURE;
        }
    }

cleanup:
    if (connected) dbctl_redis_close(&db);
    ogs_core_terminate();
    return rv;
}
