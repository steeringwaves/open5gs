---
title: Redis Subscriber Backend
---

Open5GS can store subscriber data in **Redis** instead of MongoDB. The backend is
selected at runtime from the `db_uri` scheme, so the same binaries work against
either database.

## Why Redis

MongoDB's `mongod` daemon is heavy — roughly 100–200 MB resident before any real
load. Redis is single-threaded, packaged everywhere, and uses on the order of
3 MB. On a Raspberry Pi or any small single-node 4G/5G core, Redis lets Open5GS
run as a self-contained deployment without the MongoDB footprint.

The Redis backend has full parity with the MongoDB backend for the data the
network functions use: authentication (K/OPC/AMF/SQN), AMBR, slices/sessions,
QoS and PCC rules, MSISDN lookup, IMS iFC data, and live subscriber-change
notifications to the HSS.

## Building with the Redis backend

The backend is gated by meson options (both default to `auto`):

```sh
# Auto-detects libhiredis and builds the Redis backend alongside MongoDB:
meson setup build

# Explicit:
meson setup build -Dredis=enabled

# libmongoc-free build (no MongoDB at all) — ideal for a Raspberry Pi:
meson setup build -Dmongo=disabled -Dredis=enabled
```

Dependency: `libhiredis-dev` (Debian/Ubuntu/Raspbian) or `hiredis` (Alpine/Arch).
At least one of `-Dmongo` / `-Dredis` must be enabled.

## Configuring

Point each network function's `db_uri` at a Redis URI instead of MongoDB:

```yaml
db_uri: redis://localhost/?prefix=open5gs:
```

- Host/port default to `localhost:6379`.
- `?prefix=` sets the key namespace (default `open5gs:`). Keys are
  `<prefix>subscriber:<imsi>` and `<prefix>msisdn:<bcd>`.
- `rediss://` (TLS) is **not yet supported**.

## Provisioning subscribers — `open5gs-dbctl-redis`

The MongoDB web UI is not used with Redis; provision with the `open5gs-dbctl-redis`
CLI instead. All commands take `--db-uri` (or the `DB_URI` environment variable).

```sh
# Add a subscriber (sensible defaults: APN "internet", 5QI 9, ARP 8/1/1):
open5gs-dbctl-redis --db-uri redis://localhost/ add \
    --imsi 001010000000001 \
    --key 465B5CE8B199B49FAA5F0A2EE238A6BC \
    --opc E8ED289DEBA952E4283B54E88E6183CA \
    --apn internet --sst 1 --msisdn 491725670000

open5gs-dbctl-redis --db-uri redis://localhost/ show --imsi 001010000000001
open5gs-dbctl-redis --db-uri redis://localhost/ list
open5gs-dbctl-redis --db-uri redis://localhost/ reset-sqn --imsi 001010000000001
open5gs-dbctl-redis --db-uri redis://localhost/ msisdn-add --imsi 001010000000001 --msisdn 491725670001
open5gs-dbctl-redis --db-uri redis://localhost/ msisdn-del --msisdn 491725670001
open5gs-dbctl-redis --db-uri redis://localhost/ del --imsi 001010000000001
```

For complex subscribers (IMS iFC, multiple PCC rules), edit a JSON file and use
`import` rather than command-line flags.

## Migrate subscribers from MongoDB to Redis

Export the existing subscribers from MongoDB with `mongoexport`, then import the
resulting JSON into Redis with `open5gs-dbctl-redis import`. The importer
canonicalizes MongoDB Extended JSON (e.g. `{"$numberLong":"96"}` -> `96`,
`{"$oid":"..."}` -> the plain string) so the documents are stored in the plain
JSON shape the network functions read.

```sh
mongoexport --db=open5gs --collection=subscribers --jsonArray > subs.json
open5gs-dbctl-redis --db-uri redis://localhost/ import --file subs.json
```

`import` accepts either a `--jsonArray` file (as above) or one JSON document per
line (JSONL). It reports how many subscribers were stored, maintains the
`<prefix>msisdn:<bcd>` secondary index, and publishes a change event per
subscriber so a running core picks up the new data. `export --file <path>` writes
all subscribers back out as JSONL.

## Live change propagation

For a running HSS to be notified of subscriber changes (e.g. the `add` / `del` /
`import` commands above), enable it in the HSS config (the flag name is
historical but works for both backends):

```yaml
hss:
  use_mongodb_change_stream: true
```

For Redis this relies on keyspace notifications. The HSS makes a best-effort
`CONFIG SET notify-keyspace-events Kg$` at startup; if the server denies `CONFIG`
(e.g. managed Redis), set it persistently in `redis.conf`:

```conf
notify-keyspace-events Kg$
```

The watcher listens on the rich `<prefix>events:subscriber` pub/sub channel
(precise field updates published by `open5gs-dbctl-redis`) plus the keyspace
notifications as a fallback.

## Caveats / limitations

- **Single node only** — no Redis Cluster or Sentinel support.
- **Managed Redis** that blocks `CONFIG SET` needs `notify-keyspace-events Kg$`
  set in `redis.conf` for live HSS propagation.
- **Best-effort notifications** — change events are fire-and-forget; events during
  a watcher disconnect are lost, and a Redis restart requires an HSS restart to
  re-subscribe. (MongoDB change streams have the same fire-and-forget property.)
- **`rediss://` (TLS)** is not yet supported.
- The **web UI remains MongoDB-only**; use `open5gs-dbctl-redis` for Redis.
