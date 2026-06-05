# Mongoless Open5GS (fork)

This branch replaces the MongoDB-backed subscriber catalog with a flat
YAML file plus an optional Redis state store. It is **not intended to
merge upstream** — the original MongoDB sources are kept in-tree but
gated under `#if 0` so that pulling upstream bug fixes stays painless.

- **Static subscriber data** (IMSI, security keys, slices, sessions, PCC
  rules, MSISDN, iFC) lives in a hand-edited YAML file.
- **Mutable per-subscriber state** (sqn, mme_host, mme_realm, purge_flag,
  imeisv) lives in Redis, falling back to process memory if no Redis is
  configured.
- **Hot reload**: an inotify+timerfd watcher reparses the YAML 2 s after
  the last write (trailing-edge debounce), so editor saves that touch
  the file in several syscalls only fire one reload.

The public `ogs_dbi_*` API is unchanged — HSS, UDR, PCRF, and PCF call
the same functions they always did.

---

## 1. Configuration

### `db_uri`

In every NF config (`hss.yaml`, `udr.yaml`, etc.) replace the MongoDB URI
with a path to the YAML file:

```yaml
# was:
# db_uri: mongodb://localhost/open5gs
db_uri: file:///etc/open5gs/subscribers.yaml
# or just a bare path:
# db_uri: /etc/open5gs/subscribers.yaml
```

A `mongodb://` URI is rejected at startup with a clear error so a stale
config is obvious.

### `subscribers.yaml`

Schema mirrors the original MongoDB subscriber document — field names
are identical, so a Mongo doc can be hand-translated 1:1.

A canonical example lives at
[`configs/subscribers.yaml.in`](configs/subscribers.yaml.in). Minimal
working file:

```yaml
state:
  # Optional. Omit to keep dynamic state in-memory only.
  redis: redis://127.0.0.1:6379/0

subscribers:
  - imsi: "001010000000001"
    msisdn:
      - "0000000001"
    access_restriction_data: 32
    subscriber_status: 0
    network_access_mode: 2
    subscribed_rau_tau_timer: 12

    ambr:
      downlink: { value: 1, unit: 3 }   # 1 Gbps
      uplink:   { value: 1, unit: 3 }

    security:
      k:   "465B5CE8B199B49FAA5F0A2EE238A6BC"
      opc: "E8ED289DEBA952E4283B54E88E6183CA"
      amf: "8000"
      sqn: 64

    slice:
      - sst: 1
        default_indicator: true
        session:
          - name: "internet"
            type: 3              # 1=IPv4, 2=IPv6, 3=IPv4v6
            qos:
              index: 9
              arp:
                priority_level: 8
                pre_emption_capability: 1
                pre_emption_vulnerability: 1
            ambr:
              downlink: { value: 1, unit: 3 }
              uplink:   { value: 1, unit: 3 }
```

**Bitrate units** (matches MongoDB convention): 0=bps, 1=Kbps, 2=Mbps,
3=Gbps, 4=Tbps. `value × 1000^unit`.

### State backend

```yaml
state:
  redis: redis://[:password@]host[:port][/db]
```

- `redis://127.0.0.1:6379/0` — local Redis on the default DB.
- `redis://:s3cret@redis.internal:6379/2` — with password and DB 2.
- Omit `state.redis` entirely → state lives in process memory and is
  lost on restart (fine for lab/test, breaks LTE auth resync for real
  UEs across restarts).

On Redis connection failure at boot, the daemon logs an error and
silently falls back to in-memory. The connection is reattempted on
every read/write so a Redis that comes up later starts being used
without restarting the daemon.

Redis layout: one HASH per subscriber, key
`open5gs:sub:<imsi>`, fields `sqn`, `mme_host`, `mme_realm`,
`purge_flag`, `imeisv`.

```
redis-cli> HGETALL open5gs:sub:001010000000001
1) "sqn"
2) "96"
3) "mme_host"
4) "mme0.epc.mnc070.mcc999.3gppnetwork.org"
...
```

### Hot reload

The YAML file is watched with `inotify` on its parent directory (so
atomic save-by-rename is caught even though the inode changes). After
each matching event a 2-second one-shot timer is rearmed; only when no
further event arrives for 2 seconds does the file get re-parsed.

Reloads are atomic from a reader's perspective — the cache mutex is
held for the full duration of the swap, and readers walking YAML node
pointers block briefly during it. A YAML parse error during reload is
logged and the previously-loaded catalog is kept intact.

To change the debounce window, edit `OGS_FLATFILE_WATCHER_DEBOUNCE_MS`
in [`lib/dbi/ogs-flatfile-watcher.h`](lib/dbi/ogs-flatfile-watcher.h).

---

## 2. Runtime data flow

### `subscribers.yaml` — who reads it and when

`ogs_dbi_init(db_uri)` is called **once at process start** by each of:

| NF | Call site |
|---|---|
| HSS (EPC) | [`src/hss/hss-init.c`](src/hss/hss-init.c) |
| UDR (5GC) | [`src/udr/init.c`](src/udr/init.c) |
| PCRF (EPC) | [`src/pcrf/pcrf-init.c`](src/pcrf/pcrf-init.c) |
| PCF (5GC) | [`src/pcf/init.c`](src/pcf/init.c) |

That single call parses the YAML, builds in-memory `imsi → yaml_node_t*`
and `msisdn → yaml_node_t*` indexes, and starts an inotify watcher on
the file's parent directory. **Each daemon process keeps its own
private cache and runs its own watcher** — there is no shared
read-through layer.

After startup, requests hit the in-memory cache (no disk I/O):

| NF | Functions called | Fired on |
|---|---|---|
| HSS | `ogs_dbi_auth_info` | S6a Authentication-Information-Request |
|  | `ogs_dbi_subscription_data` | S6a Update-Location-Request |
|  | `ogs_dbi_msisdn_data` | Cx / Sh by MSISDN |
|  | `ogs_dbi_ims_data` | Cx Server-Assignment-Request (iFC download) |
| UDR | `ogs_dbi_auth_info` | Nudr GET `/auth-data` from AUSF/UDM |
|  | `ogs_dbi_subscription_data` | Nudr GET `/provisioned-data` from UDM/PCF |
| PCRF | `ogs_dbi_session_data` | Gx CCR-Initial |
| PCF | `ogs_dbi_session_data` | Npcf SM-Policy Create |
|  | `ogs_dbi_subscription_data` | Npcf AM-Policy Create |

The inotify watcher fires when `subscribers.yaml` is modified;
**2 s after the last write** the cache is rebuilt under the cache
mutex. Readers block briefly during the swap. If the new file fails
to parse, the previous catalog is kept and an error is logged
([`ogs-flatfile.c`](lib/dbi/ogs-flatfile.c) `on_yaml_changed`).

### Redis state — who writes it and what

All mutable per-subscriber data lives in the Redis HASH
`open5gs:sub:<imsi>`. The set is small and write-only from a daemon
perspective:

| Field | Written by | Fired on | Implementation |
|---|---|---|---|
| `sqn` | HSS | After every successful AKA challenge over S6a | `ogs_dbi_update_sqn` ← `hss_db_update_sqn` |
|  | UDR | After every AuthEvent PATCH from UDM/AUSF | `ogs_dbi_update_sqn` ← `udr_nudr_dr_handle_subscription_authentication` |
|  | HSS / UDR | On AUTS resync (USIM detected out-of-sync) | `ogs_dbi_increment_sqn` — bumps by 32, masks to 48 bits |
| `mme_host`, `mme_realm`, `purge_flag` | **HSS only** | S6a Update-Location-Request when a UE registers to a new MME | `ogs_dbi_update_mme` ← `hss_db_update_mme` |
| `imeisv` | HSS | When the UE reports IMEISV during attach | `ogs_dbi_update_imeisv` |
|  | UDR | When UDM PUTs the UE's identity context (IMEI/IMEISV) | `ogs_dbi_update_imeisv` |

Notes:
- **5GC-only** deployments never run an HSS — only UDR writes.
- **EPC-only** deployments never run a UDR — only HSS writes.
- **Hybrid** (4G + 5G NSA/SA) — both write to the same key under their
  respective auth paths; the field semantics are identical so they
  don't clobber each other meaningfully.
- Every write goes through `ogs_flatfile_state_set_*` in
  [`lib/dbi/ogs-flatfile-state.c`](lib/dbi/ogs-flatfile-state.c) which
  writes to Redis **and** the in-memory mirror, so reads stay correct
  even if Redis is temporarily down.

### Redis state — who reads it and what for

Reads happen inside the dbi getter functions, which transparently
overlay Redis-persisted values on top of the YAML defaults:

| Field | Read by | What the value drives |
|---|---|---|
| `sqn` | `ogs_dbi_auth_info` (HSS, UDR) | Returned as the current AKA sequence number when generating the next auth vector. The USIM rejects the challenge as "synch failure" if this doesn't match its own counter — so without Redis persistence, every restart triggers an AUTS resync round-trip per UE. |
| `mme_host`, `mme_realm`, `purge_flag` | `ogs_dbi_subscription_data` (HSS) | Tells HSS where the UE was previously attached. When the same UE registers via a different MME, HSS sends a Cancel-Location-Request to the old MME using these fields; without them, the old MME would silently retain a stale UE context. |
| `imeisv` | nothing reads it back | Written for observability/auditing only. Inspect with `redis-cli HGET open5gs:sub:<imsi> imeisv` to see which device last attached as that subscriber. No daemon decisions depend on it. |

Connection handling: `ogs_flatfile_state_get_*` automatically reconnects
if Redis bounces, so a Redis restart only affects the daemon during the
brief window before reconnect — reads transparently fall back to the
in-memory mirror, and writes resume once the connection is back.

---

## 3. Building on Alpine

### Runtime + build packages

Delta from a stock Open5GS Alpine build:

| Removed | Added |
|---|---|
| `mongo-c-driver-dev` | `hiredis-dev` |
| `mongo-c-driver` (rt) | `hiredis` (rt) |

`yaml-dev` / `libyaml` are already required by upstream Open5GS — no
change there.

Full Alpine build-dep list (Alpine ≥ 3.18):

```sh
apk add --no-cache \
  build-base meson ninja pkgconf git bison flex python3 \
  libsctp-dev lksctp-tools-dev \
  yaml-dev openssl-dev \
  libgcrypt-dev libidn-dev libtalloc-dev \
  libnghttp2-dev nghttp2-dev libmicrohttpd-dev curl-dev \
  gnutls-dev \
  hiredis-dev
# (no mongo-c-driver-dev, no libbson-dev)
```

Runtime image (multi-stage `apk add` in the final layer):

```sh
apk add --no-cache \
  libsctp \
  yaml openssl \
  libgcrypt libidn libtalloc \
  libnghttp2 nghttp2 libmicrohttpd \
  gnutls \
  hiredis \
  redis            # only if you want Redis state persistence
```

### Configure + build

The backend is selected at configure time via the `mongoless` meson
option. The default is **true** (flat-file).

```sh
# YAML + Redis backend (default)
meson setup build
ninja -C build

# Original MongoDB backend
meson setup -Dmongoless=false build-mongo
ninja -C build-mongo
```

Switching an existing build dir:

```sh
meson configure build -Dmongoless=false
ninja -C build
```

Sanity check the link state:

```sh
ldd build/src/hss/open5gs-hssd | grep -iE 'mongo|bson|hiredis|yaml'
# mongoless=true  → libhiredis.so..., libyaml-0.so...
# mongoless=false → libmongoc-1.0.so..., libbson-1.0.so...
```

What the option toggles:

| | `mongoless=true` (default) | `mongoless=false` |
|---|---|---|
| `lib/dbi` extra deps | `yaml-0.1` + `hiredis` | `libmongoc-1.0` |
| Extra sources compiled | `ogs-flatfile{,-state,-watcher}.c` | none |
| `-DMONGOLESS` propagated to consumers | yes | no |
| `c_std` for `lib/dbi` | `gnu99` | project default `gnu89` |
| `tests/` subdir | skipped | built |

Everything in the source tree is gated with `#ifndef MONGOLESS` /
`#ifdef MONGOLESS`, so a single tree compiles cleanly either way.

### Dockerfile sketch

A minimal Alpine Dockerfile fragment:

```Dockerfile
FROM alpine:3.20 AS build
RUN apk add --no-cache build-base meson ninja pkgconf git bison flex python3 \
    libsctp-dev lksctp-tools-dev yaml-dev openssl-dev \
    libgcrypt-dev libidn-dev libtalloc-dev libnghttp2-dev nghttp2-dev \
    libmicrohttpd-dev curl-dev gnutls-dev hiredis-dev
WORKDIR /src
COPY . .
RUN meson setup build && ninja -C build && DESTDIR=/staging ninja -C build install

FROM alpine:3.20
RUN apk add --no-cache libsctp yaml openssl libgcrypt libidn libtalloc \
    libnghttp2 nghttp2 libmicrohttpd gnutls hiredis tini
COPY --from=build /staging /
COPY configs/subscribers.yaml.in /etc/open5gs/subscribers.yaml
ENTRYPOINT ["tini","--"]
```

Redis lives in its own container/sidecar — the HSS only needs the
`redis://` URI in its YAML.

### Notes

- The fork compiles the new sources with `c_std=gnu99` (overridden in
  [`lib/dbi/meson.build`](lib/dbi/meson.build)). The rest of the project
  stays at `gnu89`.
- Tests under `tests/` are disabled in this fork — see the `build_tests`
  gate in the top-level [`meson.build`](meson.build). They depend on
  `tests/common/context.c` which builds BSON documents directly. To
  re-enable, port that file to write YAML or Redis.
- iNotify uses an instance per process; on machines with very low
  `fs.inotify.max_user_instances` the watcher may fail to start. The
  daemon still runs — only hot reload is lost.

---

## 4. Provisioning subscribers from tooling (Go)

The struct below matches the YAML schema 1:1. Pair it with
`gopkg.in/yaml.v3` to read/write `subscribers.yaml` from any Go
provisioning tool.

```go
package open5gs

// Bitrate matches Open5GS's {value, unit} bitrate encoding.
// Unit: 0=bps, 1=Kbps, 2=Mbps, 3=Gbps, 4=Tbps.
type Bitrate struct {
    Value uint64 `yaml:"value"`
    Unit  uint8  `yaml:"unit"`
}

type AMBR struct {
    Downlink Bitrate `yaml:"downlink"`
    Uplink   Bitrate `yaml:"uplink"`
}

type ARP struct {
    PriorityLevel           uint8 `yaml:"priority_level"`
    PreEmptionCapability    uint8 `yaml:"pre_emption_capability"`
    PreEmptionVulnerability uint8 `yaml:"pre_emption_vulnerability"`
}

type QoS struct {
    Index uint8    `yaml:"index"`
    ARP   ARP      `yaml:"arp"`
    MBR   *Bitrate `yaml:"mbr,omitempty"` // PCC rule only
    GBR   *Bitrate `yaml:"gbr,omitempty"` // PCC rule only
}

// Flow describes one packet filter line of a PCC rule.
type Flow struct {
    Direction   uint8  `yaml:"direction"`   // 1 = uplink, 2 = downlink, 3 = both
    Description string `yaml:"description"` // e.g. "permit out ip from any to assigned"
}

type PCCRule struct {
    QoS  QoS    `yaml:"qos"`
    Flow []Flow `yaml:"flow,omitempty"`
}

type SMFIP struct {
    IPv4 string `yaml:"ipv4,omitempty"`
    IPv6 string `yaml:"ipv6,omitempty"`
}

type UEIP struct {
    IPv4 string `yaml:"ipv4,omitempty"`
    IPv6 string `yaml:"ipv6,omitempty"`
}

type Session struct {
    Name             string    `yaml:"name"`
    Type             uint8     `yaml:"type"` // 1=IPv4, 2=IPv6, 3=IPv4v6, 4=Unstructured, 5=Ethernet
    QoS              QoS       `yaml:"qos"`
    AMBR             AMBR      `yaml:"ambr"`
    SMF              *SMFIP    `yaml:"smf,omitempty"`
    UE               *UEIP     `yaml:"ue,omitempty"`
    IPv4FramedRoutes []string  `yaml:"ipv4_framed_routes,omitempty"`
    IPv6FramedRoutes []string  `yaml:"ipv6_framed_routes,omitempty"`
    PCCRule          []PCCRule `yaml:"pcc_rule,omitempty"`
}

type Slice struct {
    SST              uint8     `yaml:"sst"`
    SD               string    `yaml:"sd,omitempty"`              // 6-hex string; omit for sst-only S-NSSAI
    DefaultIndicator bool      `yaml:"default_indicator,omitempty"`
    Session          []Session `yaml:"session"`
}

// Security keys are stored as upper-case hex strings (same format the
// original Mongo collection uses).
type Security struct {
    K    string `yaml:"k"`              // 32 hex chars
    OPC  string `yaml:"opc,omitempty"`  // 32 hex chars (preferred)
    OP   string `yaml:"op,omitempty"`   // 32 hex chars (use OPC OR OP)
    AMF  string `yaml:"amf"`            // 4 hex chars
    RAND string `yaml:"rand,omitempty"` // 32 hex chars (test vectors only)
    SQN  uint64 `yaml:"sqn"`
}

// --- IMS (iFC) ---

type ApplicationServer struct {
    ServerName      string `yaml:"server_name"`
    DefaultHandling int    `yaml:"default_handling"`
}

type SIPHeader struct {
    Header  string `yaml:"header"`
    Content string `yaml:"content,omitempty"`
}

type SDPLine struct {
    Line    string `yaml:"line"`
    Content string `yaml:"content,omitempty"`
}

type SPT struct {
    ConditionNegated int        `yaml:"condition_negated,omitempty"`
    Group            int        `yaml:"group,omitempty"`
    Method           string     `yaml:"method,omitempty"`
    SessionCase      *int       `yaml:"session_case,omitempty"`
    SIPHeader        *SIPHeader `yaml:"sip_header,omitempty"`
    SDPLine          *SDPLine   `yaml:"sdp_line,omitempty"`
    RequestURI       string     `yaml:"request_uri,omitempty"`
}

type TriggerPoint struct {
    ConditionTypeCNF int   `yaml:"condition_type_cnf"`
    SPT              []SPT `yaml:"spt,omitempty"`
}

type IFC struct {
    Priority          int               `yaml:"priority"`
    ApplicationServer ApplicationServer `yaml:"application_server"`
    TriggerPoint      TriggerPoint      `yaml:"trigger_point"`
}

// Subscriber mirrors one entry in the `subscribers:` sequence.
type Subscriber struct {
    IMSI                       string   `yaml:"imsi"`
    MSISDN                     []string `yaml:"msisdn,omitempty"`
    IMEISV                     string   `yaml:"imeisv,omitempty"`
    AccessRestrictionData      uint32   `yaml:"access_restriction_data,omitempty"`
    SubscriberStatus           uint32   `yaml:"subscriber_status,omitempty"`
    OperatorDeterminedBarring  uint32   `yaml:"operator_determined_barring,omitempty"`
    NetworkAccessMode          uint32   `yaml:"network_access_mode,omitempty"`
    SubscribedRAUTAUTimer      uint32   `yaml:"subscribed_rau_tau_timer,omitempty"`

    AMBR     AMBR     `yaml:"ambr"`
    Security Security `yaml:"security"`
    Slice    []Slice  `yaml:"slice"`

    // These three are normally written by the HSS into the Redis state
    // store at runtime. Setting them in YAML pre-seeds the value used
    // before the first HSS write.
    MMEHost   string `yaml:"mme_host,omitempty"`
    MMERealm  string `yaml:"mme_realm,omitempty"`
    PurgeFlag bool   `yaml:"purge_flag,omitempty"`

    IFC []IFC `yaml:"ifc,omitempty"`
}

type State struct {
    Redis string `yaml:"redis,omitempty"` // redis://[:password@]host[:port][/db]
}

// Catalog is the root document of subscribers.yaml.
type Catalog struct {
    State       State        `yaml:"state,omitempty"`
    Subscribers []Subscriber `yaml:"subscribers"`
}
```

### Example: dump-and-load round trip

```go
package main

import (
    "log"
    "os"

    "gopkg.in/yaml.v3"
    "example.com/open5gs"
)

func main() {
    raw, err := os.ReadFile("/etc/open5gs/subscribers.yaml")
    if err != nil { log.Fatal(err) }

    var cat open5gs.Catalog
    if err := yaml.Unmarshal(raw, &cat); err != nil { log.Fatal(err) }

    log.Printf("loaded %d subscribers", len(cat.Subscribers))

    // Add a new SIM
    cat.Subscribers = append(cat.Subscribers, open5gs.Subscriber{
        IMSI: "001010000000999",
        MSISDN: []string{"0000000999"},
        AMBR: open5gs.AMBR{
            Downlink: open5gs.Bitrate{Value: 1, Unit: 3},
            Uplink:   open5gs.Bitrate{Value: 1, Unit: 3},
        },
        Security: open5gs.Security{
            K:   "465B5CE8B199B49FAA5F0A2EE238A6BC",
            OPC: "E8ED289DEBA952E4283B54E88E6183CA",
            AMF: "8000",
        },
        Slice: []open5gs.Slice{{
            SST: 1, DefaultIndicator: true,
            Session: []open5gs.Session{{
                Name: "internet", Type: 3,
                QoS: open5gs.QoS{Index: 9, ARP: open5gs.ARP{
                    PriorityLevel: 8,
                    PreEmptionCapability: 1,
                    PreEmptionVulnerability: 1,
                }},
                AMBR: open5gs.AMBR{
                    Downlink: open5gs.Bitrate{Value: 1, Unit: 3},
                    Uplink:   open5gs.Bitrate{Value: 1, Unit: 3},
                },
            }},
        }},
    })

    out, _ := yaml.Marshal(&cat)
    if err := os.WriteFile("/etc/open5gs/subscribers.yaml", out, 0644); err != nil {
        log.Fatal(err)
    }
    // Atomic save-and-rename is fine — the watcher tracks the directory,
    // not the inode, and debounces for 2s.
}
```

To pre-seed runtime state directly in Redis from Go, use
`github.com/redis/go-redis/v9`:

```go
rdb := redis.NewClient(&redis.Options{Addr: "127.0.0.1:6379"})
rdb.HSet(ctx, "open5gs:sub:001010000000999", map[string]any{
    "sqn": 0,
})
```

---

## 5. Migrating from a MongoDB-backed deployment

Quick mongoexport → YAML recipe:

```sh
mongoexport --db open5gs --collection subscribers --jsonArray \
  | jq '[.[] | del(._id, .__v)]' \
  | yq -P '.' -o yaml > subscribers.json.yaml
# then wrap it under a top-level `subscribers:` key
printf 'state:\n  redis: redis://127.0.0.1:6379/0\nsubscribers:\n' > subscribers.yaml
sed 's/^/  /' subscribers.json.yaml >> subscribers.yaml
```

The Mongo `_id`, `__v`, and `schema_version` fields are ignored if left
in. Everything else maps by field name.

For mutable state (`sqn`, `mme_host`, `mme_realm`, `purge_flag`,
`imeisv`), set the per-subscriber values directly in Redis once the new
HSS is running. Letting it pick them up organically on the first
attach is usually simpler.
