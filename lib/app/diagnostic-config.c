/*
 * Diagnostic configuration parser — see diagnostic-config.h for the
 * schema. Walks the global YAML document and pushes values into
 * libcore via diagnostic_broadcast_configure() and
 * diagnostic_state_configure().
 *
 * Isolated from upstream files so future v2.7.x merges only touch
 * the single call in ogs-init.c.
 */

#include "ogs-app.h"
#include "diagnostic-config.h"

#include <stdlib.h>
#include <string.h>

static void parse_broadcast(ogs_yaml_iter_t *parent,
        bool *enabled, const char **address, int *port)
{
    ogs_yaml_iter_t it;
    ogs_yaml_iter_recurse(parent, &it);
    while (ogs_yaml_iter_next(&it)) {
        const char *key = ogs_yaml_iter_key(&it);
        if (!key) continue;
        if (!strcmp(key, "enabled")) {
            *enabled = ogs_yaml_iter_bool(&it);
        } else if (!strcmp(key, "address")) {
            *address = ogs_yaml_iter_value(&it);
        } else if (!strcmp(key, "port")) {
            const char *v = ogs_yaml_iter_value(&it);
            if (v) *port = atoi(v);
        } else {
            ogs_warn("diagnostic.broadcast: unknown key '%s'", key);
        }
    }
}

static void parse_state(ogs_yaml_iter_t *parent,
        bool *enabled, const char **redis)
{
    ogs_yaml_iter_t it;
    ogs_yaml_iter_recurse(parent, &it);
    while (ogs_yaml_iter_next(&it)) {
        const char *key = ogs_yaml_iter_key(&it);
        if (!key) continue;
        if (!strcmp(key, "enabled")) {
            *enabled = ogs_yaml_iter_bool(&it);
        } else if (!strcmp(key, "redis")) {
            *redis = ogs_yaml_iter_value(&it);
        } else {
            ogs_warn("diagnostic.state: unknown key '%s'", key);
        }
    }
}

int diagnostic_config_parse(void)
{
    yaml_document_t *document;
    ogs_yaml_iter_t root_iter;

    /* Defaults — overwritten if a `diagnostic:` block is present. */
    bool bc_enabled = true;
    const char *bc_address = NULL;
    int bc_port = 0;
    bool st_enabled = false;
    const char *st_redis = NULL;
    bool found_block = false;

    document = ogs_app()->document;
    if (document) {
        ogs_yaml_iter_init(&root_iter, document);
        while (ogs_yaml_iter_next(&root_iter)) {
            const char *root_key = ogs_yaml_iter_key(&root_iter);
            if (!root_key || strcmp(root_key, "diagnostic") != 0) continue;

            found_block = true;
            {
                ogs_yaml_iter_t diag_iter;
                ogs_yaml_iter_recurse(&root_iter, &diag_iter);
                while (ogs_yaml_iter_next(&diag_iter)) {
                    const char *key = ogs_yaml_iter_key(&diag_iter);
                    if (!key) continue;
                    if (!strcmp(key, "broadcast")) {
                        parse_broadcast(&diag_iter,
                                &bc_enabled, &bc_address, &bc_port);
                    } else if (!strcmp(key, "state")) {
                        parse_state(&diag_iter, &st_enabled, &st_redis);
                    } else {
                        ogs_warn("diagnostic: unknown key '%s'", key);
                    }
                }
            }
            break;  /* a single top-level diagnostic: block */
        }
    }

    if (!found_block)
        ogs_warn("no top-level 'diagnostic:' block found in this NF's "
                "config — using defaults (broadcast on, state off). "
                "Add a `diagnostic:` block at the same indent level as "
                "your NF's main section to enable Redis state.");

    if (st_enabled && (!st_redis || !*st_redis)) {
        ogs_error("diagnostic.state.enabled is true but state.redis is empty "
                "— Redis state publishing will stay disabled");
        st_enabled = false;
    }

    /* Always configure — guarantees the boot log line fires per NF so
     * the absence of a log is itself a diagnostic signal. */
    diagnostic_broadcast_configure(bc_enabled, bc_address, bc_port);
    diagnostic_state_configure(st_enabled, st_redis);

    return OGS_OK;
}
