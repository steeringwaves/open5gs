/*
 * Mongoless: YAML-backed subscriber catalog + the public ogs_dbi_* surface.
 *
 * Walks a single YAML document at startup, caches each subscriber's yaml
 * node, and answers ogs_dbi_* queries by re-walking that node.
 *
 * Mutable per-subscriber state (sqn, mme_host/realm, imeisv, purge_flag)
 * is overlaid from the state store (ogs-flatfile-state.c) which is backed
 * by Redis when configured.
 *
 * Isolated from upstream files so upstream merges leave it untouched.
 */

#include "ogs-dbi.h"
#include "ogs-flatfile-watcher.h"

#include <yaml.h>

typedef struct flatfile_self_s {
    bool initialized;
    char *path;
    yaml_document_t document;
    bool document_loaded;
    ogs_hash_t *by_imsi;   /* imsi (bare bcd) -> yaml_node_t * */
    ogs_hash_t *by_msisdn; /* msisdn bcd      -> yaml_node_t * */

    /* Guards document + by_imsi + by_msisdn during reads (ogs_dbi_*
     * callers) vs the watcher-thread reload. Readers hold the lock for
     * their full call duration because they walk yaml_node_t pointers
     * that become invalid the moment the document is replaced. */
    ogs_thread_mutex_t cache_lock;
    bool cache_lock_init;
} flatfile_self_t;

static flatfile_self_t self;

/* ------------------------------------------------------------------ *
 * YAML helpers
 * ------------------------------------------------------------------ */

static yaml_node_t *yn_root(void) {
    return yaml_document_get_root_node(&self.document);
}

static yaml_node_t *yn_get(yaml_node_item_t id) {
    return yaml_document_get_node(&self.document, id);
}

static const char *yn_scalar(yaml_node_t *n)
{
    if (!n || n->type != YAML_SCALAR_NODE) return NULL;
    return (const char *)n->data.scalar.value;
}

static yaml_node_t *yn_map_find(yaml_node_t *map, const char *key)
{
    yaml_node_pair_t *pair;
    if (!map || map->type != YAML_MAPPING_NODE) return NULL;
    for (pair = map->data.mapping.pairs.start;
            pair < map->data.mapping.pairs.top; pair++) {
        yaml_node_t *k = yn_get(pair->key);
        const char *ks = yn_scalar(k);
        if (ks && !strcmp(ks, key))
            return yn_get(pair->value);
    }
    return NULL;
}

static const char *yn_map_str(yaml_node_t *map, const char *key)
{
    return yn_scalar(yn_map_find(map, key));
}

static int yn_map_int(yaml_node_t *map, const char *key, int def)
{
    const char *s = yn_map_str(map, key);
    return s ? atoi(s) : def;
}

static int64_t yn_map_int64(yaml_node_t *map, const char *key, int64_t def)
{
    const char *s = yn_map_str(map, key);
    return s ? strtoll(s, NULL, 10) : def;
}

static bool yn_map_bool(yaml_node_t *map, const char *key, bool def)
{
    const char *s = yn_map_str(map, key);
    if (!s) return def;
    return !strcasecmp(s, "true") || !strcmp(s, "1") || !strcasecmp(s, "yes");
}

#define YN_SEQ_FOREACH(seq, item) \
    for (yaml_node_item_t *item = (seq)->data.sequence.items.start; \
            item < (seq)->data.sequence.items.top; item++)

/* "ambr: { value: 1, unit: 3 }" → bps. unit 0=bps,1=Kbps,2=Mbps,3=Gbps,4=Tbps */
static uint64_t yn_bitrate(yaml_node_t *map)
{
    uint64_t value;
    int unit;
    int n;

    if (!map) return 0;
    value = (uint64_t)yn_map_int64(map, "value", 0);
    unit = yn_map_int(map, "unit", 0);
    for (n = 0; n < unit; n++) value *= 1000;
    return value;
}

/* ------------------------------------------------------------------ *
 * Loader
 * ------------------------------------------------------------------ */

static void flatfile_clear_indexes(void)
{
    if (self.by_imsi) {
        ogs_hash_index_t *hi;
        for (hi = ogs_hash_first(self.by_imsi); hi; hi = ogs_hash_next(hi)) {
            const void *key; int klen; void *val;
            ogs_hash_this(hi, &key, &klen, &val);
            ogs_hash_set(self.by_imsi, key, klen, NULL);
            ogs_free((void *)key);
        }
        ogs_hash_destroy(self.by_imsi);
        self.by_imsi = NULL;
    }
    if (self.by_msisdn) {
        ogs_hash_index_t *hi;
        for (hi = ogs_hash_first(self.by_msisdn); hi; hi = ogs_hash_next(hi)) {
            const void *key; int klen; void *val;
            ogs_hash_this(hi, &key, &klen, &val);
            ogs_hash_set(self.by_msisdn, key, klen, NULL);
            ogs_free((void *)key);
        }
        ogs_hash_destroy(self.by_msisdn);
        self.by_msisdn = NULL;
    }
}

static int flatfile_index_subscribers(yaml_node_t *root)
{
    yaml_node_t *subs;

    self.by_imsi = ogs_hash_make();
    self.by_msisdn = ogs_hash_make();
    ogs_assert(self.by_imsi);
    ogs_assert(self.by_msisdn);

    subs = yn_map_find(root, "subscribers");
    if (!subs || subs->type != YAML_SEQUENCE_NODE) {
        ogs_error("flatfile: 'subscribers' is missing or not a sequence");
        return OGS_ERROR;
    }

    YN_SEQ_FOREACH(subs, item) {
        yaml_node_t *sub = yn_get(*item);
        const char *imsi;

        if (!sub || sub->type != YAML_MAPPING_NODE) continue;

        imsi = yn_map_str(sub, "imsi");
        if (!imsi) {
            ogs_warn("flatfile: subscriber entry without imsi — skipping");
            continue;
        }

        char *key = ogs_strdup(imsi);
        ogs_assert(key);
        ogs_hash_set(self.by_imsi, key, strlen(key), sub);

        yaml_node_t *msisdns = yn_map_find(sub, "msisdn");
        if (msisdns && msisdns->type == YAML_SEQUENCE_NODE) {
            YN_SEQ_FOREACH(msisdns, m_item) {
                const char *m = yn_scalar(yn_get(*m_item));
                if (!m) continue;
                char *mkey = ogs_strdup(m);
                ogs_assert(mkey);
                ogs_hash_set(self.by_msisdn, mkey, strlen(mkey), sub);
            }
        }
    }
    return OGS_OK;
}

int ogs_flatfile_reload(const char *path)
{
    FILE *fp;
    yaml_parser_t parser;
    yaml_document_t doc;

    ogs_assert(path);

    fp = fopen(path, "rb");
    if (!fp) {
        ogs_error("flatfile: cannot open %s: %s", path, strerror(errno));
        return OGS_ERROR;
    }

    if (!yaml_parser_initialize(&parser)) {
        ogs_error("flatfile: yaml_parser_initialize failed");
        fclose(fp);
        return OGS_ERROR;
    }
    yaml_parser_set_input_file(&parser, fp);

    if (!yaml_parser_load(&parser, &doc)) {
        ogs_error("flatfile: YAML parse error at line %zu col %zu: %s",
                parser.problem_mark.line + 1,
                parser.problem_mark.column + 1,
                parser.problem ? parser.problem : "(unknown)");
        yaml_parser_delete(&parser);
        fclose(fp);
        return OGS_ERROR;
    }
    yaml_parser_delete(&parser);
    fclose(fp);

    flatfile_clear_indexes();
    if (self.document_loaded) {
        yaml_document_delete(&self.document);
        self.document_loaded = false;
    }

    self.document = doc;
    self.document_loaded = true;

    yaml_node_t *root = yn_root();
    if (!root || root->type != YAML_MAPPING_NODE) {
        ogs_error("flatfile: root must be a YAML mapping");
        return OGS_ERROR;
    }

    return flatfile_index_subscribers(root);
}

static yaml_node_t *flatfile_lookup_imsi(const char *imsi)
{
    if (!self.by_imsi || !imsi) return NULL;
    return ogs_hash_get(self.by_imsi, imsi, strlen(imsi));
}

static yaml_node_t *flatfile_lookup_imsi_or_msisdn(const char *bcd)
{
    yaml_node_t *n;
    if (!bcd) return NULL;
    n = self.by_imsi ? ogs_hash_get(self.by_imsi, bcd, strlen(bcd)) : NULL;
    if (n) return n;
    n = self.by_msisdn ? ogs_hash_get(self.by_msisdn, bcd, strlen(bcd)) : NULL;
    return n;
}

/* ------------------------------------------------------------------ *
 * Public ogs_dbi_* API
 * ------------------------------------------------------------------ */

/* Snapshot the IMSI key set from a live by_imsi hash into a fresh hash
 * whose values are just the sentinel 0x1. Caller frees with
 * free_imsi_snapshot(). Used to diff pre-reload vs post-reload so we can
 * reconcile Redis state for removed subscribers. */
static ogs_hash_t *snapshot_imsi_keys(ogs_hash_t *src)
{
    ogs_hash_t *snap = ogs_hash_make();
    ogs_hash_index_t *hi;

    ogs_assert(snap);
    if (!src) return snap;

    for (hi = ogs_hash_first(src); hi; hi = ogs_hash_next(hi)) {
        const void *key;
        int klen;
        void *val;
        char *kcopy;
        ogs_hash_this(hi, &key, &klen, &val);
        kcopy = ogs_strndup(key, klen);
        ogs_assert(kcopy);
        ogs_hash_set(snap, kcopy, klen, (void *)(uintptr_t)1);
    }
    return snap;
}

static void free_imsi_snapshot(ogs_hash_t *snap)
{
    ogs_hash_index_t *hi;
    if (!snap) return;
    for (hi = ogs_hash_first(snap); hi; hi = ogs_hash_next(hi)) {
        const void *key;
        int klen;
        void *val;
        ogs_hash_this(hi, &key, &klen, &val);
        ogs_hash_set(snap, key, klen, NULL);
        ogs_free((void *)key);
    }
    ogs_hash_destroy(snap);
}

/* For every IMSI in old_keys that isn't in self.by_imsi anymore, drop
 * its Redis state. Caller must hold self.cache_lock. */
static void reconcile_removed_subscribers(ogs_hash_t *old_keys)
{
    ogs_hash_index_t *hi;
    int removed = 0;

    if (!old_keys || !self.by_imsi) return;

    for (hi = ogs_hash_first(old_keys); hi; hi = ogs_hash_next(hi)) {
        const void *key;
        int klen;
        void *val;
        char *imsi;
        ogs_hash_this(hi, &key, &klen, &val);
        if (ogs_hash_get(self.by_imsi, key, klen)) continue;

        imsi = ogs_strndup(key, klen);
        ogs_assert(imsi);
        ogs_info("flatfile: subscriber %s removed from YAML — "
                "dropping Redis state", imsi);
        ogs_flatfile_state_remove(imsi);
        ogs_free(imsi);
        removed++;
    }
    if (removed)
        ogs_info("flatfile: reconciled %d removed subscriber(s)", removed);
}

/* Watcher callback. Holds the cache lock for the full reload — readers
 * arriving during the swap block briefly. */
static void on_yaml_changed(void)
{
    int rv;
    ogs_hash_t *old_imsi_set;
    unsigned int old_count, new_count;

    ogs_thread_mutex_lock(&self.cache_lock);

    /* Snapshot must happen BEFORE reload — ogs_flatfile_reload() clears
     * self.by_imsi as it rebuilds. */
    old_imsi_set = snapshot_imsi_keys(self.by_imsi);
    old_count = self.by_imsi ? ogs_hash_count(self.by_imsi) : 0;

    rv = ogs_flatfile_reload(self.path);
    if (rv == OGS_OK) {
        ogs_info("flatfile: reloaded %s", self.path);

        new_count = self.by_imsi ? ogs_hash_count(self.by_imsi) : 0;

        /* Tripwire: if the new YAML has zero subscribers and the old one
         * had any, refuse to reconcile — almost certainly an accidental
         * empty/truncated file. Same logic for >50%-drop catches the
         * "forgot to escape a key" class of accidents. */
        if (old_count > 0 && new_count == 0) {
            ogs_warn("flatfile: new YAML has 0 subscribers but old had %u — "
                    "refusing to auto-purge Redis state. Inspect %s and use "
                    "`valkey-cli DEL` manually if this was intentional.",
                    old_count, self.path);
        } else if (old_count >= 4 && new_count * 2 < old_count) {
            ogs_warn("flatfile: new YAML has %u subscribers (down from %u) — "
                    "more than half removed. Skipping auto-purge of Redis "
                    "state as a safety measure; reconcile manually.",
                    new_count, old_count);
        } else {
            reconcile_removed_subscribers(old_imsi_set);
        }
    } else {
        ogs_error("flatfile: reload of %s failed — catalog may be partial",
                self.path);
    }

    free_imsi_snapshot(old_imsi_set);

    ogs_thread_mutex_unlock(&self.cache_lock);
}

int ogs_dbi_init(const char *db_uri)
{
    const char *path = db_uri;
    yaml_node_t *root, *state;
    const char *redis_uri = NULL;
    int rv;

    if (!db_uri) {
        ogs_error("flatfile: no db_uri configured");
        return OGS_ERROR;
    }

    /* Accept "file:///path" or a bare path. Reject the legacy mongo URI
     * with a clear message so misconfig is obvious. */
    if (!strncmp(db_uri, "mongodb://", 10) ||
        !strncmp(db_uri, "mongodb+srv://", 14)) {
        ogs_error("flatfile: db_uri is a MongoDB URI but mongo support is "
                "disabled in this build. Point db_uri at a YAML file instead.");
        return OGS_ERROR;
    }
    if (!strncmp(db_uri, "file://", 7))
        path = db_uri + 7;

    memset(&self, 0, sizeof(self));
    ogs_thread_mutex_init(&self.cache_lock);
    self.cache_lock_init = true;

    self.path = ogs_strdup(path);
    ogs_assert(self.path);

    rv = ogs_flatfile_reload(self.path);
    if (rv != OGS_OK) {
        ogs_free(self.path);
        self.path = NULL;
        ogs_thread_mutex_destroy(&self.cache_lock);
        self.cache_lock_init = false;
        return rv;
    }

    root = yn_root();
    state = yn_map_find(root, "state");
    if (state && state->type == YAML_MAPPING_NODE)
        redis_uri = yn_map_str(state, "redis");

    rv = ogs_flatfile_state_init(redis_uri);
    if (rv != OGS_OK) {
        ogs_error("flatfile: state backend init failed");
        return rv;
    }

    /* Start the file watcher last — a daemon should run even if inotify
     * setup fails (e.g. on a system with the user_watches limit hit). */
    if (ogs_flatfile_watcher_init(self.path, on_yaml_changed) != OGS_OK)
        ogs_warn("flatfile: file watcher disabled, hot reload unavailable");

    self.initialized = true;
    ogs_info("flatfile: loaded %s", self.path);
    return OGS_OK;
}

void ogs_dbi_final(void)
{
    if (!self.initialized) return;

    /* Stop the watcher first so no reload races with teardown. */
    ogs_flatfile_watcher_final();

    ogs_flatfile_state_final();

    ogs_thread_mutex_lock(&self.cache_lock);
    flatfile_clear_indexes();
    if (self.document_loaded) {
        yaml_document_delete(&self.document);
        self.document_loaded = false;
    }
    ogs_thread_mutex_unlock(&self.cache_lock);

    if (self.path) {
        ogs_free(self.path);
        self.path = NULL;
    }

    if (self.cache_lock_init) {
        ogs_thread_mutex_destroy(&self.cache_lock);
        self.cache_lock_init = false;
    }

    self.initialized = false;
}

/* HSS uses these for hot-reload via MongoDB change streams. No-op stubs. */
int ogs_dbi_collection_watch_init(void) { return OGS_OK; }
int ogs_dbi_poll_change_stream(void) { return OGS_OK; }

/* ------------------------------------------------------------------ *
 * Auth info (security keys + sqn)
 * ------------------------------------------------------------------ */

int ogs_dbi_auth_info(char *supi, ogs_dbi_auth_info_t *auth_info)
{
    char *supi_type = NULL, *supi_id = NULL;
    yaml_node_t *sub, *sec;
    const char *s;
    uint64_t sqn_override;
    int rv = OGS_OK;

    ogs_assert(supi);
    ogs_assert(auth_info);

    supi_type = ogs_id_get_type(supi);
    if (!supi_type) { ogs_error("Invalid supi=%s", supi); return OGS_ERROR; }
    supi_id = ogs_id_get_value(supi);
    if (!supi_id) {
        ogs_error("Invalid supi=%s", supi);
        ogs_free(supi_type);
        return OGS_ERROR;
    }

    ogs_thread_mutex_lock(&self.cache_lock);

    sub = flatfile_lookup_imsi(supi_id);
    if (!sub) {
        ogs_info("[%s] Cannot find IMSI in DB", supi);
        rv = OGS_ERROR;
        goto out;
    }

    sec = yn_map_find(sub, OGS_SECURITY_STRING);
    if (!sec || sec->type != YAML_MAPPING_NODE) {
        ogs_error("[%s] missing security block", supi);
        rv = OGS_ERROR;
        goto out;
    }

    memset(auth_info, 0, sizeof(*auth_info));

    if ((s = yn_map_str(sec, OGS_K_STRING)))
        ogs_ascii_to_hex((char *)s, strlen(s),
                auth_info->k, OGS_KEY_LEN);

    if ((s = yn_map_str(sec, OGS_OPC_STRING))) {
        auth_info->use_opc = 1;
        ogs_ascii_to_hex((char *)s, strlen(s),
                auth_info->opc, OGS_KEY_LEN);
    }
    if ((s = yn_map_str(sec, OGS_OP_STRING)))
        ogs_ascii_to_hex((char *)s, strlen(s),
                auth_info->op, OGS_KEY_LEN);
    if ((s = yn_map_str(sec, OGS_AMF_STRING)))
        ogs_ascii_to_hex((char *)s, strlen(s),
                auth_info->amf, OGS_AMF_LEN);
    if ((s = yn_map_str(sec, OGS_RAND_STRING)))
        ogs_ascii_to_hex((char *)s, strlen(s),
                auth_info->rand, OGS_RAND_LEN);

    auth_info->sqn = (uint64_t)yn_map_int64(sec, OGS_SQN_STRING, 0);

    /* State-store overlay wins over YAML default. ogs_flatfile_state_*
     * has its own internal lock — cache_lock is held across the call
     * but the two locks are independent and always acquired in this
     * order, so deadlock is impossible. */
    if (ogs_flatfile_state_get_sqn(supi_id, &sqn_override) == OGS_OK)
        auth_info->sqn = sqn_override;

out:
    ogs_thread_mutex_unlock(&self.cache_lock);
    ogs_free(supi_type);
    ogs_free(supi_id);
    return rv;
}

int ogs_dbi_update_sqn(char *supi, uint64_t sqn)
{
    char *supi_id;
    int rv;
    ogs_assert(supi);
    supi_id = ogs_id_get_value(supi);
    ogs_assert(supi_id);
    rv = ogs_flatfile_state_set_sqn(supi_id, sqn);
    ogs_free(supi_id);
    return rv;
}

int ogs_dbi_increment_sqn(char *supi)
{
    char *supi_id;
    int rv;
    ogs_assert(supi);
    supi_id = ogs_id_get_value(supi);
    ogs_assert(supi_id);
    rv = ogs_flatfile_state_increment_sqn(supi_id);
    ogs_free(supi_id);
    return rv;
}

int ogs_dbi_update_imeisv(char *supi, char *imeisv)
{
    char *supi_id;
    int rv;
    ogs_assert(supi);
    supi_id = ogs_id_get_value(supi);
    ogs_assert(supi_id);
    rv = ogs_flatfile_state_set_imeisv(supi_id, imeisv);
    ogs_free(supi_id);
    return rv;
}

int ogs_dbi_update_mme(char *supi, char *mme_host, char *mme_realm,
        bool purge_flag)
{
    char *supi_id;
    int rv;
    ogs_assert(supi);
    supi_id = ogs_id_get_value(supi);
    ogs_assert(supi_id);
    rv = ogs_flatfile_state_set_mme(supi_id, mme_host, mme_realm, purge_flag);
    ogs_free(supi_id);
    return rv;
}

/* ------------------------------------------------------------------ *
 * Subscription data (everything the UDR/HSS needs to provision a UE)
 * ------------------------------------------------------------------ */

static void load_session_pcc_rules(yaml_node_t *pcc_rules_seq,
        ogs_session_data_t *session_data, const char *dnn);
static void load_session(yaml_node_t *sess_map, ogs_session_t *session);

static void load_slice(yaml_node_t *slice_map, ogs_slice_data_t *slice_data)
{
    yaml_node_t *sessions;
    const char *s;

    slice_data->s_nssai.sst = (uint8_t)yn_map_int(slice_map, OGS_SST_STRING, 0);
    slice_data->s_nssai.sd.v = OGS_S_NSSAI_NO_SD_VALUE;
    s = yn_map_str(slice_map, OGS_SD_STRING);
    if (s) slice_data->s_nssai.sd = ogs_s_nssai_sd_from_string(s);
    slice_data->default_indicator =
            yn_map_bool(slice_map, OGS_DEFAULT_INDICATOR_STRING, false);

    sessions = yn_map_find(slice_map, OGS_SESSION_STRING);
    if (!sessions || sessions->type != YAML_SEQUENCE_NODE) return;

    YN_SEQ_FOREACH(sessions, item) {
        yaml_node_t *sess_map = yn_get(*item);
        ogs_session_t *session;

        if (!sess_map || sess_map->type != YAML_MAPPING_NODE) continue;
        if (slice_data->num_of_session >= OGS_MAX_NUM_OF_SESS) break;

        session = &slice_data->session[slice_data->num_of_session++];
        load_session(sess_map, session);
    }
}

static void load_session(yaml_node_t *sess_map, ogs_session_t *session)
{
    yaml_node_t *qos, *arp, *ambr, *smf, *ue, *routes;
    const char *s;

    s = yn_map_str(sess_map, OGS_NAME_STRING);
    if (s) { session->name = ogs_strdup(s); ogs_assert(session->name); }

    session->session_type = (uint8_t)yn_map_int(sess_map, OGS_TYPE_STRING, 0);

    /* 2.7.7+ field; defaults to false when omitted. */
    session->lbo_roaming_allowed =
            yn_map_bool(sess_map, OGS_LBO_ROAMING_ALLOWED_STRING, false);

    qos = yn_map_find(sess_map, OGS_QOS_STRING);
    if (qos && qos->type == YAML_MAPPING_NODE) {
        session->qos.index = yn_map_int(qos, OGS_INDEX_STRING, 0);
        arp = yn_map_find(qos, OGS_ARP_STRING);
        if (arp && arp->type == YAML_MAPPING_NODE) {
            session->qos.arp.priority_level =
                    yn_map_int(arp, OGS_PRIORITY_LEVEL_STRING, 0);
            session->qos.arp.pre_emption_capability =
                    yn_map_int(arp, OGS_PRE_EMPTION_CAPABILITY_STRING, 0);
            session->qos.arp.pre_emption_vulnerability =
                    yn_map_int(arp, OGS_PRE_EMPTION_VULNERABILITY_STRING, 0);
        }
    }

    ambr = yn_map_find(sess_map, OGS_AMBR_STRING);
    if (ambr && ambr->type == YAML_MAPPING_NODE) {
        session->ambr.downlink = yn_bitrate(yn_map_find(ambr, OGS_DOWNLINK_STRING));
        session->ambr.uplink = yn_bitrate(yn_map_find(ambr, OGS_UPLINK_STRING));
    }

    smf = yn_map_find(sess_map, OGS_SMF_STRING);
    if (smf && smf->type == YAML_MAPPING_NODE) {
        const char *v;
        ogs_ipsubnet_t sub;
        v = yn_map_str(smf, OGS_IPV4_STRING);
        if (v && ogs_ipsubnet(&sub, v, NULL) == OGS_OK) {
            session->smf_ip.ipv4 = 1;
            session->smf_ip.addr = sub.sub[0];
        }
        v = yn_map_str(smf, OGS_IPV6_STRING);
        if (v && ogs_ipsubnet(&sub, v, NULL) == OGS_OK) {
            session->smf_ip.ipv6 = 1;
            memcpy(session->smf_ip.addr6, sub.sub, sizeof(sub.sub));
        }
    }

    ue = yn_map_find(sess_map, OGS_UE_STRING);
    if (ue && ue->type == YAML_MAPPING_NODE) {
        const char *v;
        ogs_ipsubnet_t sub;
        v = yn_map_str(ue, OGS_IPV4_STRING);
        if (v && ogs_ipsubnet(&sub, v, NULL) == OGS_OK) {
            session->ue_ip.ipv4 = true;
            session->ue_ip.addr = sub.sub[0];
        }
        v = yn_map_str(ue, OGS_IPV6_STRING);
        if (v && ogs_ipsubnet(&sub, v, NULL) == OGS_OK) {
            session->ue_ip.ipv6 = true;
            memcpy(session->ue_ip.addr6, sub.sub, OGS_IPV6_LEN);
        }
    }

    routes = yn_map_find(sess_map, OGS_IPV4_FRAMED_ROUTES_STRING);
    if (routes && routes->type == YAML_SEQUENCE_NODE) {
        int i = 0;
        session->ipv4_framed_routes = ogs_calloc(
                OGS_MAX_NUM_OF_FRAMED_ROUTES_IN_PDI,
                sizeof(session->ipv4_framed_routes[0]));
        ogs_assert(session->ipv4_framed_routes);
        YN_SEQ_FOREACH(routes, ri) {
            const char *v;
            if (i >= OGS_MAX_NUM_OF_FRAMED_ROUTES_IN_PDI) break;
            v = yn_scalar(yn_get(*ri));
            if (!v) continue;
            session->ipv4_framed_routes[i++] = ogs_strdup(v);
        }
    }
    routes = yn_map_find(sess_map, OGS_IPV6_FRAMED_ROUTES_STRING);
    if (routes && routes->type == YAML_SEQUENCE_NODE) {
        int i = 0;
        session->ipv6_framed_routes = ogs_calloc(
                OGS_MAX_NUM_OF_FRAMED_ROUTES_IN_PDI,
                sizeof(session->ipv6_framed_routes[0]));
        ogs_assert(session->ipv6_framed_routes);
        YN_SEQ_FOREACH(routes, ri) {
            const char *v;
            if (i >= OGS_MAX_NUM_OF_FRAMED_ROUTES_IN_PDI) break;
            v = yn_scalar(yn_get(*ri));
            if (!v) continue;
            session->ipv6_framed_routes[i++] = ogs_strdup(v);
        }
    }
}

int ogs_dbi_subscription_data(char *supi,
        ogs_subscription_data_t *subscription_data)
{
    char *supi_type = NULL, *supi_id = NULL;
    yaml_node_t *sub;
    const char *s;
    yaml_node_t *msisdns, *ambr, *slices;
    char *mme_host = NULL, *mme_realm = NULL;
    bool purge_flag = false;
    int rv = OGS_OK;

    ogs_assert(supi);
    ogs_assert(subscription_data);

    memset(subscription_data, 0, sizeof(*subscription_data));

    supi_type = ogs_id_get_type(supi); ogs_assert(supi_type);
    supi_id = ogs_id_get_value(supi); ogs_assert(supi_id);

    ogs_thread_mutex_lock(&self.cache_lock);

    sub = flatfile_lookup_imsi(supi_id);
    if (!sub) {
        ogs_error("[%s] Cannot find IMSI in DB", supi);
        rv = OGS_ERROR;
        goto out;
    }

    s = yn_map_str(sub, OGS_IMSI_STRING);
    if (s) {
        subscription_data->imsi = ogs_strdup(s);
        ogs_assert(subscription_data->imsi);
    }

    msisdns = yn_map_find(sub, OGS_MSISDN_STRING);
    if (msisdns && msisdns->type == YAML_SEQUENCE_NODE) {
        int idx = 0;
        YN_SEQ_FOREACH(msisdns, mi) {
            const char *m;
            if (idx >= OGS_MAX_NUM_OF_MSISDN) break;
            m = yn_scalar(yn_get(*mi));
            if (!m) continue;
            ogs_cpystrn(subscription_data->msisdn[idx].bcd, m,
                    ogs_min(strlen(m), OGS_MAX_MSISDN_BCD_LEN) + 1);
            ogs_bcd_to_buffer(subscription_data->msisdn[idx].bcd,
                    subscription_data->msisdn[idx].buf,
                    &subscription_data->msisdn[idx].len);
            idx++;
        }
        subscription_data->num_of_msisdn = idx;
    }

    subscription_data->access_restriction_data = (uint32_t)
            yn_map_int(sub, OGS_ACCESS_RESTRICTION_DATA_STRING, 0);
    subscription_data->subscriber_status = (uint32_t)
            yn_map_int(sub, OGS_SUBSCRIBER_STATUS_STRING, 0);
    subscription_data->operator_determined_barring = (uint32_t)
            yn_map_int(sub, OGS_OPERATOR_DETERMINED_BARRING_STRING, 0);
    subscription_data->network_access_mode = (uint32_t)
            yn_map_int(sub, OGS_NETWORK_ACCESS_MODE_STRING, 0);
    subscription_data->subscribed_rau_tau_timer = (uint32_t)
            yn_map_int(sub, OGS_SUBSCRIBED_RAU_TAU_TIMER_STRING,
                    OGS_RAU_TAU_DEFAULT_TIME);

    ambr = yn_map_find(sub, OGS_AMBR_STRING);
    if (ambr && ambr->type == YAML_MAPPING_NODE) {
        subscription_data->ambr.downlink =
                yn_bitrate(yn_map_find(ambr, OGS_DOWNLINK_STRING));
        subscription_data->ambr.uplink =
                yn_bitrate(yn_map_find(ambr, OGS_UPLINK_STRING));
    }

    slices = yn_map_find(sub, OGS_SLICE_STRING);
    if (slices && slices->type == YAML_SEQUENCE_NODE) {
        YN_SEQ_FOREACH(slices, si) {
            yaml_node_t *slice_map = yn_get(*si);
            ogs_slice_data_t *slice_data;

            if (!slice_map || slice_map->type != YAML_MAPPING_NODE) continue;
            if (subscription_data->num_of_slice >= OGS_MAX_NUM_OF_SLICE) break;
            slice_data = &subscription_data->slice[
                    subscription_data->num_of_slice];
            slice_data->s_nssai.sst = 0;
            slice_data->s_nssai.sd.v = OGS_S_NSSAI_NO_SD_VALUE;
            load_slice(slice_map, slice_data);

            if (slice_data->s_nssai.sst == 0) {
                ogs_error("No SST");
                continue;
            }
            subscription_data->num_of_slice++;
        }
    }

    /* Static MME info from YAML, then overlay from state store. */
    s = yn_map_str(sub, OGS_MME_HOST_STRING);
    if (s) {
        subscription_data->mme_host = ogs_strdup(s);
        ogs_assert(subscription_data->mme_host);
    }
    s = yn_map_str(sub, OGS_MME_REALM_STRING);
    if (s) {
        subscription_data->mme_realm = ogs_strdup(s);
        ogs_assert(subscription_data->mme_realm);
    }
    subscription_data->purge_flag =
            yn_map_bool(sub, OGS_PURGE_FLAG_STRING, false);

    if (ogs_flatfile_state_get_mme(supi_id, &mme_host, &mme_realm, &purge_flag)
            == OGS_OK) {
        if (mme_host) {
            if (subscription_data->mme_host)
                ogs_free(subscription_data->mme_host);
            subscription_data->mme_host = mme_host;
        }
        if (mme_realm) {
            if (subscription_data->mme_realm)
                ogs_free(subscription_data->mme_realm);
            subscription_data->mme_realm = mme_realm;
        }
        subscription_data->purge_flag = purge_flag;
    }

out:
    ogs_thread_mutex_unlock(&self.cache_lock);
    ogs_free(supi_type);
    ogs_free(supi_id);
    return rv;
}

/* ------------------------------------------------------------------ *
 * Session data (per-DNN session + PCC rules)
 * ------------------------------------------------------------------ */

static void load_session_pcc_rules(yaml_node_t *pcc_rules_seq,
        ogs_session_data_t *session_data, const char *dnn)
{
    int i, pcc_index = 0;

    for (i = 0; i < session_data->num_of_pcc_rule; i++)
        OGS_PCC_RULE_FREE(&session_data->pcc_rule[i]);

    YN_SEQ_FOREACH(pcc_rules_seq, item) {
        yaml_node_t *rule_map = yn_get(*item);
        ogs_pcc_rule_t *pcc_rule;
        yaml_node_t *qos, *arp, *mbr, *gbr, *flows;

        if (!rule_map || rule_map->type != YAML_MAPPING_NODE) continue;
        if (pcc_index >= OGS_MAX_NUM_OF_PCC_RULE) break;
        pcc_rule = &session_data->pcc_rule[pcc_index];

        qos = yn_map_find(rule_map, OGS_QOS_STRING);
        if (qos && qos->type == YAML_MAPPING_NODE) {
            pcc_rule->qos.index = yn_map_int(qos, OGS_INDEX_STRING, 0);
            arp = yn_map_find(qos, OGS_ARP_STRING);
            if (arp && arp->type == YAML_MAPPING_NODE) {
                pcc_rule->qos.arp.priority_level =
                        yn_map_int(arp, OGS_PRIORITY_LEVEL_STRING, 0);
                pcc_rule->qos.arp.pre_emption_capability =
                        yn_map_int(arp, OGS_PRE_EMPTION_CAPABILITY_STRING, 0);
                pcc_rule->qos.arp.pre_emption_vulnerability =
                        yn_map_int(arp, OGS_PRE_EMPTION_VULNERABILITY_STRING, 0);
            }
            mbr = yn_map_find(qos, OGS_MBR_STRING);
            if (mbr && mbr->type == YAML_MAPPING_NODE) {
                pcc_rule->qos.mbr.downlink =
                        yn_bitrate(yn_map_find(mbr, OGS_DOWNLINK_STRING));
                pcc_rule->qos.mbr.uplink =
                        yn_bitrate(yn_map_find(mbr, OGS_UPLINK_STRING));
            }
            gbr = yn_map_find(qos, OGS_GBR_STRING);
            if (gbr && gbr->type == YAML_MAPPING_NODE) {
                pcc_rule->qos.gbr.downlink =
                        yn_bitrate(yn_map_find(gbr, OGS_DOWNLINK_STRING));
                pcc_rule->qos.gbr.uplink =
                        yn_bitrate(yn_map_find(gbr, OGS_UPLINK_STRING));
            }
        }

        flows = yn_map_find(rule_map, OGS_FLOW_STRING);
        if (flows && flows->type == YAML_SEQUENCE_NODE) {
            int flow_index = 0;
            YN_SEQ_FOREACH(flows, fi) {
                yaml_node_t *flow_map = yn_get(*fi);
                ogs_flow_t *flow;
                const char *v;

                if (!flow_map || flow_map->type != YAML_MAPPING_NODE) continue;
                if (flow_index >= OGS_MAX_NUM_OF_FLOW_IN_PCC_RULE) break;
                flow = &pcc_rule->flow[flow_index];

                flow->direction = yn_map_int(flow_map, OGS_DIRECTION_STRING, 0);
                v = yn_map_str(flow_map, OGS_DESCRIPTION_STRING);
                if (v) {
                    size_t vlen = strlen(v);
                    flow->description = ogs_calloc(1, vlen + 1);
                    ogs_assert(flow->description);
                    ogs_cpystrn((char *)flow->description, v, vlen + 1);
                }
                flow_index++;
            }
            pcc_rule->num_of_flow = flow_index;
        }

        ogs_assert(!pcc_rule->name);
        pcc_rule->name = ogs_msprintf("%s-g%d", dnn, pcc_index + 1);
        ogs_assert(pcc_rule->name);
        ogs_assert(!pcc_rule->id);
        pcc_rule->id = ogs_msprintf("%s-n%d", dnn, pcc_index + 1);
        ogs_assert(pcc_rule->id);
        pcc_rule->precedence = pcc_index + 1;
        pcc_index++;
    }
    session_data->num_of_pcc_rule = pcc_index;
}

int ogs_dbi_session_data(
        const char *supi, const ogs_s_nssai_t *s_nssai, const char *dnn,
        ogs_session_data_t *session_data)
{
    char *supi_type = NULL, *supi_id = NULL;
    yaml_node_t *sub, *slices;
    yaml_node_t *matched_session = NULL;
    int rv = OGS_OK;

    ogs_assert(supi);
    ogs_assert(dnn);
    ogs_assert(session_data);

    supi_type = ogs_id_get_type(supi); ogs_assert(supi_type);
    supi_id = ogs_id_get_value(supi); ogs_assert(supi_id);

    ogs_thread_mutex_lock(&self.cache_lock);

    sub = flatfile_lookup_imsi(supi_id);
    if (!sub) {
        ogs_error("[%s] Cannot find IMSI in DB", supi);
        rv = OGS_ERROR;
        goto out;
    }

    slices = yn_map_find(sub, OGS_SLICE_STRING);
    if (!slices || slices->type != YAML_SEQUENCE_NODE) {
        ogs_error("[%s] no slice array", supi);
        rv = OGS_ERROR;
        goto out;
    }

    YN_SEQ_FOREACH(slices, si) {
        yaml_node_t *slice_map = yn_get(*si);
        uint8_t sst;
        ogs_uint24_t sd;
        const char *sd_str;
        yaml_node_t *sessions;

        if (!slice_map || slice_map->type != YAML_MAPPING_NODE) continue;

        sst = (uint8_t)yn_map_int(slice_map, OGS_SST_STRING, 0);
        sd.v = OGS_S_NSSAI_NO_SD_VALUE;
        sd_str = yn_map_str(slice_map, OGS_SD_STRING);
        if (sd_str) sd = ogs_s_nssai_sd_from_string(sd_str);

        if (sst == 0) { ogs_error("No SST"); continue; }
        if (s_nssai && s_nssai->sst != sst) continue;
        if (s_nssai &&
            s_nssai->sd.v != OGS_S_NSSAI_NO_SD_VALUE &&
            sd.v != OGS_S_NSSAI_NO_SD_VALUE &&
            s_nssai->sd.v != sd.v) continue;

        sessions = yn_map_find(slice_map, OGS_SESSION_STRING);
        if (!sessions || sessions->type != YAML_SEQUENCE_NODE) continue;

        YN_SEQ_FOREACH(sessions, sessi) {
            yaml_node_t *sess_map = yn_get(*sessi);
            const char *name;
            if (!sess_map || sess_map->type != YAML_MAPPING_NODE) continue;
            name = yn_map_str(sess_map, OGS_NAME_STRING);
            if (!name) continue;
            if (ogs_strncasecmp(name, dnn, strlen(name)) == 0 &&
                    strlen(name) == strlen(dnn)) {
                matched_session = sess_map;
                goto found;
            }
        }
    }

found:
    if (!matched_session) {
        ogs_error("Cannot find SUPI[%s] S_NSSAI[SST:%d SD:0x%x] DNN[%s] in DB",
                supi_id,
                s_nssai ? s_nssai->sst : 0,
                s_nssai ? s_nssai->sd.v : 0,
                dnn);
        rv = OGS_ERROR;
        goto out;
    }

    /* Free pre-existing session->name if any. */
    if (session_data->session.name) {
        ogs_free(session_data->session.name);
        session_data->session.name = NULL;
    }
    load_session(matched_session, &session_data->session);

    {
        yaml_node_t *pcc_rules =
                yn_map_find(matched_session, OGS_PCC_RULE_STRING);
        if (pcc_rules && pcc_rules->type == YAML_SEQUENCE_NODE)
            load_session_pcc_rules(pcc_rules, session_data, dnn);
    }

out:
    ogs_thread_mutex_unlock(&self.cache_lock);
    ogs_free(supi_type);
    ogs_free(supi_id);
    return rv;
}

/* ------------------------------------------------------------------ *
 * MSISDN / IMS data
 * ------------------------------------------------------------------ */

int ogs_dbi_msisdn_data(
        char *imsi_or_msisdn_bcd, ogs_msisdn_data_t *msisdn_data)
{
    yaml_node_t *sub;
    yaml_node_t *msisdns;
    const char *s;
    int rv = OGS_OK;

    ogs_assert(imsi_or_msisdn_bcd);
    ogs_assert(msisdn_data);
    memset(msisdn_data, 0, sizeof(*msisdn_data));

    ogs_thread_mutex_lock(&self.cache_lock);

    sub = flatfile_lookup_imsi_or_msisdn(imsi_or_msisdn_bcd);
    if (!sub) {
        ogs_error("[%s] Cannot find IMSI or MSISDN in DB", imsi_or_msisdn_bcd);
        rv = OGS_ERROR;
        goto out;
    }

    s = yn_map_str(sub, OGS_IMSI_STRING);
    if (s) {
        ogs_cpystrn(msisdn_data->imsi.bcd, s,
                ogs_min(strlen(s), OGS_MAX_IMSI_BCD_LEN) + 1);
        ogs_bcd_to_buffer(msisdn_data->imsi.bcd,
                msisdn_data->imsi.buf, &msisdn_data->imsi.len);
    }

    msisdns = yn_map_find(sub, OGS_MSISDN_STRING);
    if (msisdns && msisdns->type == YAML_SEQUENCE_NODE) {
        int idx = 0;
        YN_SEQ_FOREACH(msisdns, mi) {
            const char *m;
            if (idx >= OGS_MAX_NUM_OF_MSISDN) break;
            m = yn_scalar(yn_get(*mi));
            if (!m) continue;
            ogs_cpystrn(msisdn_data->msisdn[idx].bcd, m,
                    ogs_min(strlen(m), OGS_MAX_MSISDN_BCD_LEN) + 1);
            ogs_bcd_to_buffer(msisdn_data->msisdn[idx].bcd,
                    msisdn_data->msisdn[idx].buf,
                    &msisdn_data->msisdn[idx].len);
            idx++;
        }
        msisdn_data->num_of_msisdn = idx;
    }
out:
    ogs_thread_mutex_unlock(&self.cache_lock);
    return rv;
}

/* IMS / iFC parsing — covers msisdn + the iFC array used by the HSS Cx path.
 * Returns OK with empty IFC if the YAML omits it (the rest of the HSS still
 * gets the msisdn list it needs). */
int ogs_dbi_ims_data(char *supi, ogs_ims_data_t *ims_data)
{
    char *supi_type = NULL, *supi_id = NULL;
    yaml_node_t *sub, *msisdns, *ifcs;
    int rv = OGS_OK;

    ogs_assert(supi);
    ogs_assert(ims_data);
    memset(ims_data, 0, sizeof(*ims_data));

    supi_type = ogs_id_get_type(supi); ogs_assert(supi_type);
    supi_id = ogs_id_get_value(supi); ogs_assert(supi_id);

    ogs_thread_mutex_lock(&self.cache_lock);

    sub = flatfile_lookup_imsi(supi_id);
    if (!sub) {
        ogs_error("[%s] Cannot find IMSI in DB", supi);
        rv = OGS_ERROR;
        goto out;
    }

    msisdns = yn_map_find(sub, OGS_MSISDN_STRING);
    if (msisdns && msisdns->type == YAML_SEQUENCE_NODE) {
        int idx = 0;
        YN_SEQ_FOREACH(msisdns, mi) {
            const char *m;
            if (idx >= OGS_MAX_NUM_OF_MSISDN) break;
            m = yn_scalar(yn_get(*mi));
            if (!m) continue;
            ogs_cpystrn(ims_data->msisdn[idx].bcd, m,
                    ogs_min(strlen(m), OGS_MAX_MSISDN_BCD_LEN) + 1);
            ogs_bcd_to_buffer(ims_data->msisdn[idx].bcd,
                    ims_data->msisdn[idx].buf,
                    &ims_data->msisdn[idx].len);
            idx++;
        }
        ims_data->num_of_msisdn = idx;
    }

    ifcs = yn_map_find(sub, "ifc");
    if (ifcs && ifcs->type == YAML_SEQUENCE_NODE) {
        int ifc_index = 0;
        YN_SEQ_FOREACH(ifcs, ifci) {
            yaml_node_t *ifc_map = yn_get(*ifci);
            yaml_node_t *as, *tp, *spts;
            ogs_ifc_t *ifc;
            const char *s;

            if (!ifc_map || ifc_map->type != YAML_MAPPING_NODE) continue;
            if (ifc_index >= OGS_MAX_NUM_OF_IFC) break;
            ifc = &ims_data->ifc[ifc_index];

            ifc->priority = yn_map_int(ifc_map, "priority", 0);

            as = yn_map_find(ifc_map, "application_server");
            if (as && as->type == YAML_MAPPING_NODE) {
                s = yn_map_str(as, "server_name");
                if (s) ifc->application_server.server_name = ogs_strdup(s);
                ifc->application_server.default_handling =
                        yn_map_int(as, "default_handling", 0);
            }

            tp = yn_map_find(ifc_map, "trigger_point");
            if (tp && tp->type == YAML_MAPPING_NODE) {
                ifc->trigger_point.condition_type_cnf =
                        yn_map_int(tp, "condition_type_cnf", 0);
                spts = yn_map_find(tp, "spt");
                if (spts && spts->type == YAML_SEQUENCE_NODE) {
                    int spt_idx = 0;
                    YN_SEQ_FOREACH(spts, spti) {
                        yaml_node_t *spt_map = yn_get(*spti);
                        yaml_node_t *header, *sdp;

                        if (!spt_map || spt_map->type != YAML_MAPPING_NODE)
                            continue;
                        if (spt_idx >= OGS_MAX_NUM_OF_SPT) break;

                        ifc->trigger_point.spt[spt_idx].condition_negated =
                                yn_map_int(spt_map, "condition_negated", 0);
                        ifc->trigger_point.spt[spt_idx].group =
                                yn_map_int(spt_map, "group", 0);

                        s = yn_map_str(spt_map, "method");
                        if (s) {
                            ifc->trigger_point.spt[spt_idx].method =
                                    ogs_strdup(s);
                            ifc->trigger_point.spt[spt_idx].type =
                                    OGS_SPT_HAS_METHOD;
                        }
                        if (yn_map_find(spt_map, "session_case")) {
                            ifc->trigger_point.spt[spt_idx].session_case =
                                    yn_map_int(spt_map, "session_case", 0);
                            ifc->trigger_point.spt[spt_idx].type =
                                    OGS_SPT_HAS_SESSION_CASE;
                        }

                        header = yn_map_find(spt_map, "sip_header");
                        if (header && header->type == YAML_MAPPING_NODE) {
                            s = yn_map_str(header, "header");
                            if (s) ifc->trigger_point.spt[spt_idx].header =
                                    ogs_strdup(s);
                            s = yn_map_str(header, "content");
                            if (s) ifc->trigger_point.spt[spt_idx].header_content
                                    = ogs_strdup(s);
                            ifc->trigger_point.spt[spt_idx].type =
                                    OGS_SPT_HAS_SIP_HEADER;
                        }
                        sdp = yn_map_find(spt_map, "sdp_line");
                        if (sdp && sdp->type == YAML_MAPPING_NODE) {
                            s = yn_map_str(sdp, "line");
                            if (s) ifc->trigger_point.spt[spt_idx].sdp_line =
                                    ogs_strdup(s);
                            s = yn_map_str(sdp, "content");
                            if (s) ifc->trigger_point.spt[spt_idx].sdp_line_content
                                    = ogs_strdup(s);
                            ifc->trigger_point.spt[spt_idx].type =
                                    OGS_SPT_HAS_SDP_LINE;
                        }
                        s = yn_map_str(spt_map, "request_uri");
                        if (s) {
                            ifc->trigger_point.spt[spt_idx].request_uri =
                                    ogs_strdup(s);
                            ifc->trigger_point.spt[spt_idx].type =
                                    OGS_SPT_HAS_REQUEST_URI;
                        }
                        spt_idx++;
                    }
                    ifc->trigger_point.num_of_spt = spt_idx;
                }
            }
            ifc_index++;
        }
        ims_data->num_of_ifc = ifc_index;
    }

out:
    ogs_thread_mutex_unlock(&self.cache_lock);
    ogs_free(supi_type);
    ogs_free(supi_id);
    return rv;
}
