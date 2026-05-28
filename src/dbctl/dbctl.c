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

/* A command that is recognized but not yet implemented in this task. */
static int cmd_not_implemented(const char *name)
{
    ogs_error("'%s' is not implemented yet", name);
    return OGS_ERROR;
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

    if (!strcmp(cmd, "ping")) {
        rv = (cmd_ping(&db) == OGS_OK) ? EXIT_SUCCESS : EXIT_FAILURE;
    } else if (!strcmp(cmd, "add") ||
               !strcmp(cmd, "del") ||
               !strcmp(cmd, "show") ||
               !strcmp(cmd, "list") ||
               !strcmp(cmd, "import") ||
               !strcmp(cmd, "export") ||
               !strcmp(cmd, "msisdn-add") ||
               !strcmp(cmd, "msisdn-del") ||
               !strcmp(cmd, "reset-sqn")) {
        rv = (cmd_not_implemented(cmd) == OGS_OK) ?
                EXIT_SUCCESS : EXIT_FAILURE;
    } else {
        ogs_error("Unknown command: %s", cmd);
        usage(stderr);
        rv = EXIT_FAILURE;
    }

cleanup:
    if (connected) dbctl_redis_close(&db);
    ogs_core_terminate();
    return rv;
}
