/*
 * Mongoless: inotify-based file watcher for the YAML subscriber catalog,
 * with a trailing-edge debounce window so a noisy save (e.g. editor that
 * truncates → writes → fsyncs in several syscalls) only triggers one
 * reload.
 *
 * Isolated from upstream files.
 */

#if !defined(OGS_DBI_INSIDE) && !defined(OGS_DBI_COMPILATION)
#error "This header cannot be included directly."
#endif

#ifndef OGS_DBI_FLATFILE_WATCHER_H
#define OGS_DBI_FLATFILE_WATCHER_H

#ifdef __cplusplus
extern "C" {
#endif

#define OGS_FLATFILE_WATCHER_DEBOUNCE_MS 2000

/*
 * Start watching `path`. On a settled change, on_change() runs from the
 * watcher thread; the callback is responsible for any locking required
 * to swap the in-memory catalog safely.
 *
 * Returns OGS_OK on success, OGS_ERROR if inotify/timerfd setup fails.
 * If the watcher cannot be initialised the daemon should still run with
 * the catalog it loaded at boot.
 */
int ogs_flatfile_watcher_init(const char *path, void (*on_change)(void));

/* Stop the watcher thread and release resources. Safe to call when the
 * watcher was never started or failed to start. */
void ogs_flatfile_watcher_final(void);

#ifdef __cplusplus
}
#endif

#endif /* OGS_DBI_FLATFILE_WATCHER_H */
