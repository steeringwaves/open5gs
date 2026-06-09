/*
 * Diagnostic configuration — parses the top-level `diagnostic:` block
 * out of the global YAML config and pushes the values into libcore's
 * diagnostic-broadcast and diagnostic-state modules.
 *
 * Isolated into its own pair of files so future merges against upstream
 * v2.7.x only need a single one-line call from ogs-init.c.
 *
 * Schema:
 *
 *   diagnostic:
 *     broadcast:
 *       enabled: true                       # default true
 *       address: 127.0.0.199                # default
 *       port: 2287                          # default
 *     state:
 *       enabled: false                      # default false (opt-in)
 *       redis: redis://127.0.0.1:6379/1     # required when enabled
 *
 * Both blocks are optional — omitting either preserves upstream
 * behaviour (broadcast on, state off).
 */

#if !defined(OGS_APP_INSIDE) && !defined(OGS_APP_COMPILATION)
#error "This header cannot be included directly."
#endif

#ifndef DIAGNOSTIC_CONFIG_H
#define DIAGNOSTIC_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Walk the global YAML document (ogs_app()->document) for the
 * `diagnostic:` block and configure the underlying libcore modules.
 * Returns OGS_OK on success, OGS_ERROR if the YAML is malformed in a
 * way we can't recover from. A missing block is OK — defaults stand.
 */
int diagnostic_config_parse(void);

#ifdef __cplusplus
}
#endif

#endif /* DIAGNOSTIC_CONFIG_H */
