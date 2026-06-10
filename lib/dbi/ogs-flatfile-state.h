/*
 * Mongoless: per-subscriber mutable state store (sqn, mme_host, imeisv,
 * purge_flag) used by ogs_dbi_* writers. Backed by Redis when a redis://
 * URI is configured, otherwise kept in memory only.
 *
 * Isolated from upstream files so upstream merges leave it untouched.
 */

#if !defined(OGS_DBI_INSIDE) && !defined(OGS_DBI_COMPILATION)
#error "This header cannot be included directly."
#endif

#ifndef OGS_DBI_FLATFILE_STATE_H
#define OGS_DBI_FLATFILE_STATE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Connect to the state backend. uri == NULL → in-memory only. */
int ogs_flatfile_state_init(const char *uri);
void ogs_flatfile_state_final(void);

/* SQN. Returns OGS_OK and sets *out_sqn if present; OGS_NOTFOUND otherwise. */
int ogs_flatfile_state_get_sqn(const char *imsi, uint64_t *out_sqn);
int ogs_flatfile_state_set_sqn(const char *imsi, uint64_t sqn);
int ogs_flatfile_state_increment_sqn(const char *imsi);

/* MME host/realm + purge flag. Returns OGS_OK and assigns out_host / out_realm
 * (caller must ogs_free) and out_purge_flag when present. */
int ogs_flatfile_state_get_mme(const char *imsi,
        char **out_host, char **out_realm, bool *out_purge_flag);
int ogs_flatfile_state_set_mme(const char *imsi,
        const char *host, const char *realm, bool purge_flag);

/* IMEISV. Returns OGS_OK and assigns *out_imeisv (caller must ogs_free). */
int ogs_flatfile_state_get_imeisv(const char *imsi, char **out_imeisv);
int ogs_flatfile_state_set_imeisv(const char *imsi, const char *imeisv);

/* Drop all state for this IMSI. DEL the Redis HASH and free the in-memory
 * mirror entry. Idempotent — safe to call on IMSIs we've never seen. */
int ogs_flatfile_state_remove(const char *imsi);

#ifdef __cplusplus
}
#endif

#endif /* OGS_DBI_FLATFILE_STATE_H */
