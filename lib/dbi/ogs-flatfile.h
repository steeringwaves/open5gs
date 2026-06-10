/*
 * Mongoless: YAML-backed subscriber catalog loader.
 *
 * Replaces ogs-mongoc.{c,h} + subscription.c + session.c + ims.c on this
 * fork. The MongoDB sources are kept verbatim (under #if 0) so upstream
 * cherry-picks land cleanly.
 *
 * Public ogs_dbi_* API surface is preserved — callers in HSS/UDR/PCRF/PCF
 * are unchanged.
 *
 * db_uri format (ogs_app()->db_uri):
 *   /path/to/subscribers.yaml          (any path, no scheme)
 *   file:///path/to/subscribers.yaml   (file:// scheme)
 *
 * The YAML file itself may declare an optional Redis state backend:
 *   state:
 *     redis: redis://127.0.0.1:6379/0
 *   subscribers:
 *     - imsi: "001010000000001"
 *       ...
 */

#if !defined(OGS_DBI_INSIDE) && !defined(OGS_DBI_COMPILATION)
#error "This header cannot be included directly."
#endif

#ifndef OGS_DBI_FLATFILE_H
#define OGS_DBI_FLATFILE_H

#include "ogs-flatfile-state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Implemented in ogs-flatfile.c. */
int ogs_flatfile_reload(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* OGS_DBI_FLATFILE_H */
