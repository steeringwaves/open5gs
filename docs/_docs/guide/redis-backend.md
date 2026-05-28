---
title: Redis Subscriber Backend
---

Open5GS can store subscriber data in Redis instead of MongoDB. Point the network
functions at a Redis URI (`db_uri: redis://localhost/?prefix=open5gs:`) and use
the `open5gs-dbctl-redis` CLI to provision subscribers.

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
subscriber so a running core picks up the new data.

## Live change propagation

For a running HSS/UDM to be notified of subscriber changes (e.g. the `add` /
`del` / `import` commands above) the Redis server must have keyspace
notifications enabled:

```sh
redis-cli CONFIG SET notify-keyspace-events 'Kg$'
```

This is the keyspace-event fallback the watcher uses in addition to the rich
`<prefix>events:subscriber` pub/sub channel (see the Phase 3 change-notification
design). Set it persistently in `redis.conf` for production deployments.
