#!/bin/sh
#
# Copyright (C) 2026 by Sukchan Lee <acetcom@gmail.com>
#
# This file is part of Open5GS.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
# Docker-gated runner for the Redis dbi equivalence/round-trip test.
#
# Spins up a throwaway redis:7-alpine container on a random high port,
# provisions the canonical subscriber fixture + the MSISDN secondary index via
# redis-cli, exports OGS_TEST_REDIS_URI and runs the dbi test suite, then tears
# the container down. Exits with the meson test exit code.
#
# Skips cleanly (exit 0) if docker is not available, so CI without Docker stays
# green (the equivalence test itself also skips when OGS_TEST_REDIS_URI is unset).

set -e

# Resolve the repo root from this script's location (tests/dbi/..) so the
# script works regardless of the caller's CWD.
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
cd "$REPO_ROOT"

BUILD_DIR="build-phase1"
FIXTURE="tests/dbi/fixtures/subscriber.json"
PREFIX="test:"
IMSI="001010000000001"
MSISDN="491725670000"
REDIS_IMAGE="redis:7-alpine"

if ! command -v docker >/dev/null 2>&1; then
    echo "run-redis-tests: docker not found; skipping Redis integration test."
    echo "(The equivalence test will skip cleanly without OGS_TEST_REDIS_URI.)"
    exit 0
fi

if [ ! -f "$FIXTURE" ]; then
    echo "run-redis-tests: fixture $FIXTURE not found." >&2
    exit 1
fi

port=$(shuf -i 20000-29999 -n1)
echo "run-redis-tests: starting $REDIS_IMAGE on 127.0.0.1:$port"
cid=$(docker run -d --rm -p "127.0.0.1:$port:6379" "$REDIS_IMAGE")

cleanup() {
    echo "run-redis-tests: stopping container $cid"
    docker stop "$cid" >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

# Wait until Redis answers PONG (or time out).
echo "run-redis-tests: waiting for Redis to become reachable..."
ready=0
i=0
while [ "$i" -lt 30 ]; do
    if [ "$(docker exec "$cid" redis-cli ping 2>/dev/null)" = "PONG" ]; then
        ready=1
        break
    fi
    i=$((i + 1))
    sleep 1
done
if [ "$ready" -ne 1 ]; then
    echo "run-redis-tests: Redis did not become ready in time." >&2
    exit 1
fi

# Enable keyspace notifications explicitly so the watcher's keyspace fallback
# fires deterministically for the live watcher test. The backend also tries this
# at watch_init (best-effort), but setting it here removes any doubt.
#   K = keyspace events, g = generic commands (DEL/EXPIRE/...), $ = string ops
#       (SET lives here), so the test's SET produces a __keyspace@..__ event.
echo "run-redis-tests: enabling keyspace notifications (notify-keyspace-events Kg\$)"
docker exec "$cid" redis-cli CONFIG SET notify-keyspace-events 'Kg$' >/dev/null

# Provision the canonical subscriber JSON and the MSISDN secondary index.
# The JSON value must match what redis-equivalence-test.c asserts (sqn 96 etc.).
echo "run-redis-tests: provisioning fixture (subscriber + msisdn index)"
docker exec -i "$cid" redis-cli -x SET "${PREFIX}subscriber:${IMSI}" \
    < "$FIXTURE" >/dev/null
docker exec "$cid" redis-cli SET "${PREFIX}msisdn:${MSISDN}" "$IMSI" >/dev/null

OGS_TEST_REDIS_URI="redis://127.0.0.1:${port}/?prefix=${PREFIX}"
export OGS_TEST_REDIS_URI
echo "run-redis-tests: OGS_TEST_REDIS_URI=$OGS_TEST_REDIS_URI"

# Run the dbi suite. Disable -e around the test so we can capture its exit code
# and still tear the container down.
set +e
meson test -C "$BUILD_DIR" --suite dbi -v
rv=$?
set -e

echo "run-redis-tests: meson test exit code $rv"
exit $rv
