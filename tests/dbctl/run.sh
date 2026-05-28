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
# Docker-gated live integration test for the open5gs-dbctl-redis CLI.
#
# Spins up a throwaway redis:7-alpine container on a random high port (prefix
# "test:") and exercises the CLI end-to-end:
#
#   1. add        -> raw redis-cli check of the written subscriber + msisdn index
#   2. add        -> read-back through the BACKEND: meson --suite dbctl runs the
#                    ogs_dbi_* integration case, proving CLI-writes == NF-reads
#   3. import     -> "imported 2", list shows both imsis, show canonicalizes the
#                    mongoexport $numberLong sqn to a plain number
#   4. del        -> the subscriber key and its msisdn index are gone (nil)
#
# Then tears the container down (trap EXIT). Skips cleanly (exit 0) when docker
# is not available so CI without Docker stays green.

set -e

# Resolve the repo root from this script's own location (tests/dbctl/..) so the
# script works regardless of the caller's CWD.
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
cd "$REPO_ROOT"

BUILD_DIR="build-phase1"
CLI="$BUILD_DIR/src/dbctl/open5gs-dbctl-redis"
FIXTURE="tests/dbctl/fixtures/mongoexport.json"
PREFIX="test:"
IMSI="001010000000001"
IMSI2="001010000000002"
KEY="465B5CE8B199B49FAA5F0A2EE238A6BC"
OPC="E8ED289DEBA952E4283B54E88E6183CA"
MSISDN="491725670000"
REDIS_IMAGE="redis:7-alpine"

if ! command -v docker >/dev/null 2>&1; then
    echo "run: docker not found; skipping dbctl live integration test."
    echo "(The backend read-back case skips cleanly without OGS_TEST_REDIS_URI.)"
    exit 0
fi

if [ ! -x "$CLI" ]; then
    echo "run: CLI $CLI not found/executable; build it first" >&2
    echo "     (ninja -C $BUILD_DIR open5gs-dbctl-redis)" >&2
    exit 1
fi
if [ ! -f "$FIXTURE" ]; then
    echo "run: fixture $FIXTURE not found." >&2
    exit 1
fi

fail() { echo "run: FAIL: $*" >&2; exit 1; }

port=$(shuf -i 20000-29999 -n1)
echo "run: starting $REDIS_IMAGE on 127.0.0.1:$port"
cid=$(docker run -d --rm -p "127.0.0.1:$port:6379" "$REDIS_IMAGE")

cleanup() {
    echo "run: stopping container $cid"
    docker stop "$cid" >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

# Wait until Redis answers PONG (or time out).
echo "run: waiting for Redis to become reachable..."
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
[ "$ready" -eq 1 ] || fail "Redis did not become ready in time"

# notify-keyspace-events Kg$ so the CLI's publish-change events have somewhere
# to go (cross-ref Phase 3); not strictly needed by this test but mirrors prod.
docker exec "$cid" redis-cli CONFIG SET notify-keyspace-events 'Kg$' >/dev/null

URI="redis://127.0.0.1:${port}/?prefix=${PREFIX}"
rcli() { docker exec "$cid" redis-cli "$@"; }

# ---------------------------------------------------------------------------
echo
echo "=== Step 1: add -> raw redis-cli check ==="
"$CLI" --db-uri "$URI" add \
    --imsi "$IMSI" --key "$KEY" --opc "$OPC" \
    --apn internet --sst 1 --msisdn "$MSISDN"

sub=$(rcli GET "${PREFIX}subscriber:${IMSI}")
echo "raw subscriber:${IMSI} = $sub"
[ -n "$sub" ] || fail "subscriber key is empty after add"
echo "$sub" | grep -q "\"imsi\":\"${IMSI}\"" || fail "subscriber JSON missing imsi"
echo "$sub" | grep -q "\"k\":\"${KEY}\""     || fail "subscriber JSON missing k"
echo "$sub" | grep -q "\"opc\":\"${OPC}\""   || fail "subscriber JSON missing opc"
echo "$sub" | grep -q "\"name\":\"internet\"" || fail "subscriber JSON missing apn"

mi=$(rcli GET "${PREFIX}msisdn:${MSISDN}")
echo "raw msisdn:${MSISDN} = $mi"
[ "$mi" = "$IMSI" ] || fail "msisdn index != imsi (got '$mi')"
echo "Step 1 OK: raw subscriber + msisdn index match what 'add' wrote."

# ---------------------------------------------------------------------------
echo
echo "=== Step 2: add -> read-back through the BACKEND (ogs_dbi_*) ==="
echo "    meson --suite dbctl reads ${PREFIX}subscriber:${IMSI} via ogs_dbi_*"
OGS_TEST_REDIS_URI="$URI"
export OGS_TEST_REDIS_URI
set +e
meson test -C "$BUILD_DIR" --suite dbctl -v
mrv=$?
set -e
unset OGS_TEST_REDIS_URI
[ "$mrv" -eq 0 ] || fail "backend read-back meson test failed (exit $mrv)"
echo "Step 2 OK: ogs_dbi_auth_info/subscription_data read exactly what the CLI wrote."

# ---------------------------------------------------------------------------
echo
echo "=== Step 3: import -> list / show (canonicalized sqn) ==="
imp=$("$CLI" --db-uri "$URI" import --file "$FIXTURE")
echo "$imp"
echo "$imp" | grep -q "imported 2" || fail "import did not report 'imported 2'"

lst=$("$CLI" --db-uri "$URI" list)
echo "list output:"
echo "$lst"
echo "$lst" | grep -qx "$IMSI"  || fail "list missing $IMSI"
echo "$lst" | grep -qx "$IMSI2" || fail "list missing $IMSI2"

shown=$("$CLI" --db-uri "$URI" show --imsi "$IMSI")
echo "show --imsi $IMSI:"
echo "$shown"
# The mongoexport fixture wrapped sqn as {"$numberLong":"96"}; import must have
# canonicalized it to a plain number. Assert a bare numeric sqn and NO wrapper.
echo "$shown" | grep -Eq '"sqn":[[:space:]]*96\b' \
    || fail "show did not canonicalize sqn to the number 96"
echo "$shown" | grep -q 'numberLong' \
    && fail "show still contains a \$numberLong wrapper (not canonicalized)"
echo "Step 3 OK: imported 2, list shows both, show renders sqn as a plain number."

# ---------------------------------------------------------------------------
echo
echo "=== Step 4: del -> keys gone ==="
"$CLI" --db-uri "$URI" del --imsi "$IMSI"

gone_sub=$(rcli GET "${PREFIX}subscriber:${IMSI}")
gone_mi=$(rcli GET "${PREFIX}msisdn:${MSISDN}")
echo "after del: subscriber='$gone_sub' msisdn='$gone_mi'"
[ -z "$gone_sub" ] || fail "subscriber key still present after del"
[ -z "$gone_mi" ]  || fail "msisdn index still present after del"
echo "Step 4 OK: subscriber key and msisdn index are gone (nil)."

echo
echo "run: ALL STEPS PASSED"
exit 0
