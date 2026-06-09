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

static int parse_broadcast(ogs_yaml_iter_t *parent)
{
    ogs_yaml_iter_t it;
    bool enabled = true;
    const char *address = NULL;
    int port = 0;

    ogs_yaml_iter_recurse(parent, &it);
    while (ogs_yaml_iter_next(&it)) {
        const char *key = ogs_yaml_iter_key(&it);
        if (!key) continue;
        if (!strcmp(key, "enabled")) {
            enabled = ogs_yaml_iter_bool(&it);
        } else if (!strcmp(key, "address")) {
            address = ogs_yaml_iter_value(&it);
        } else if (!strcmp(key, "port")) {
            const char *v = ogs_yaml_iter_value(&it);
            if (v) port = atoi(v);
        } else {
            ogs_warn("diagnostic.broadcast: unknown key '%s'", key);
        }
    }

    diagnostic_broadcast_configure(enabled, address, port);
    return OGS_OK;
}

static int parse_state(ogs_yaml_iter_t *parent)
{
    ogs_yaml_iter_t it;
    bool enabled = false;
    const char *redis = NULL;

    ogs_yaml_iter_recurse(parent, &it);
    while (ogs_yaml_iter_next(&it)) {
        const char *key = ogs_yaml_iter_key(&it);
        if (!key) continue;
        if (!strcmp(key, "enabled")) {
            enabled = ogs_yaml_iter_bool(&it);
        } else if (!strcmp(key, "redis")) {
            redis = ogs_yaml_iter_value(&it);
        } else {
            ogs_warn("diagnostic.state: unknown key '%s'", key);
        }
    }

    if (enabled && (!redis || !*redis)) {
        ogs_error("diagnostic.state.enabled is true but state.redis is empty "
                "— Redis state publishing will stay disabled");
        enabled = false;
    }

    diagnostic_state_configure(enabled, redis);
    return OGS_OK;
}

int diagnostic_config_parse(void)
{
    yaml_document_t *document;
    ogs_yaml_iter_t root_iter;

    document = ogs_app()->document;
    if (!document) return OGS_OK;  /* no YAML loaded — keep defaults */

    ogs_yaml_iter_init(&root_iter, document);
    while (ogs_yaml_iter_next(&root_iter)) {
        const char *root_key = ogs_yaml_iter_key(&root_iter);
        if (!root_key || strcmp(root_key, "diagnostic") != 0) continue;

        {
            ogs_yaml_iter_t diag_iter;
            ogs_yaml_iter_recurse(&root_iter, &diag_iter);
            while (ogs_yaml_iter_next(&diag_iter)) {
                const char *key = ogs_yaml_iter_key(&diag_iter);
                if (!key) continue;
                if (!strcmp(key, "broadcast")) {
                    parse_broadcast(&diag_iter);
                } else if (!strcmp(key, "state")) {
                    parse_state(&diag_iter);
                } else {
                    ogs_warn("diagnostic: unknown key '%s'", key);
                }
            }
        }
        break;  /* a single top-level diagnostic: block */
    }

    return OGS_OK;
}
