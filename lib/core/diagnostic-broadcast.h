#ifndef DIAGNOSTIC_BROADCAST_H
#define DIAGNOSTIC_BROADCAST_H
extern void diagnostic_broadcast_internal(const char *fmt, ...);

// Macro for easier use
#define diagnostic_broadcast(fmt, ...)                                         \
  diagnostic_broadcast_internal(fmt, ##__VA_ARGS__)

#endif