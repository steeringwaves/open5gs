#ifndef DIAGNOSTIC_BROADCAST_H
#define DIAGNOSTIC_BROADCAST_H

#include <stdbool.h>

extern void diagnostic_broadcast_internal(const char *fmt, ...);

/*
 * Runtime configuration. Called from lib/app/diagnostic-config.c after
 * the global YAML has been parsed. Each NULL/0 argument keeps the
 * current default:
 *   enabled = true
 *   address = "127.0.0.199"
 *   port    = 2287
 *
 * Safe to call multiple times (the last call wins).
 */
extern void diagnostic_broadcast_configure(bool enabled,
        const char *address, int port);

/* Macro for easier use */
#define diagnostic_broadcast(fmt, ...)                                         \
  diagnostic_broadcast_internal(fmt, ##__VA_ARGS__)

#endif