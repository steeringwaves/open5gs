/*
 * Diagnostic state — live per-entity keys in Redis, complementing the
 * fire-and-forget UDP diagnostic_broadcast() events.
 *
 * Each event type has a set/del pair. set() creates/refreshes a HASH at
 * a well-known key path; del() removes it. Reading the live system
 * state then becomes a Valkey/Redis scan instead of a stream parse.
 *
 * Opt-in: configured at boot from the global YAML's `diagnostic.state`
 * block — see lib/app/diagnostic-config.{c,h}. Until
 * diagnostic_state_configure(enabled=true, ...) is called, every helper
 * is a silent no-op.
 *
 * URL format (parsed identically to lib/dbi/ogs-flatfile-state.c):
 *   redis://[:password@]host[:port][/db]
 *
 * Key schema:
 *   open5gs:live:gnb:<address>          HASH { address, connected_at }
 *   open5gs:live:enb:<address>          HASH { address, connected_at }
 *   open5gs:live:ue:<imsi>              HASH { imsi, imei, supi, suci, attached_at }
 *   open5gs:live:session:<imsi>:<apn>   HASH { imsi, apn, imei, supi, ipv4, ipv6, created_at }
 *
 * All helpers are thread-safe (a single internal mutex serialises Redis
 * I/O — same trade-off as ogs-flatfile-state.c).
 */

#ifndef DIAGNOSTIC_STATE_H
#define DIAGNOSTIC_STATE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Log domain — shows up as [diag] in daemon logs. Installed from
 * ogs_core_initialize() at boot. */
extern int __ogs_diag_domain;

void diagnostic_state_init(void);
void diagnostic_state_final(void);

/*
 * Runtime configuration. Called from lib/app/diagnostic-config.c after
 * the global YAML has been parsed. With enabled=false (or redis_url
 * NULL/""), every set/del helper becomes a silent no-op.
 *
 * Safe to call multiple times; the existing connection is closed
 * before the new URL is honoured.
 */
void diagnostic_state_configure(bool enabled, const char *redis_url);

/* Radio access nodes. address is the SCTP peer address as an
 * already-formatted IPv4/IPv6 string. */
void diagnostic_state_gnb_set(const char *address);
void diagnostic_state_gnb_del(const char *address);
void diagnostic_state_enb_set(const char *address);
void diagnostic_state_enb_del(const char *address);

/* Subscriber attach state. Any NULL identity field is stored as "". */
void diagnostic_state_ue_set(const char *imsi, const char *imei,
                             const char *supi, const char *suci);
void diagnostic_state_ue_del(const char *imsi);

/* Per-session state. apn is required; either IPv4 or IPv6 may be NULL/"". */
void diagnostic_state_session_set(const char *imsi, const char *apn,
                                  const char *imei, const char *supi,
                                  const char *ipv4, const char *ipv6);
void diagnostic_state_session_del(const char *imsi, const char *apn);

#ifdef __cplusplus
}
#endif

#endif /* DIAGNOSTIC_STATE_H */
