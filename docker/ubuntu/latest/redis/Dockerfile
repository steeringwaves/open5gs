# Redis-only (libmongoc-free) Open5GS build example.
#
# Demonstrates building Open5GS WITHOUT MongoDB — suitable for a Raspberry Pi or
# any small single-node core. It uses the Redis backend (db_uri: redis://...)
# and ships the open5gs-dbctl-redis provisioning CLI. Note the dependency list
# has NO libmongoc-dev / libbson-dev.
#
# Build from the repository root:
#   docker build -f docker/ubuntu/latest/redis/Dockerfile -t open5gs-redis .
#
# See docs/_docs/guide/redis-backend.md for configuration and provisioning.

FROM ubuntu:24.04

LABEL org.opencontainers.image.authors="Open5GS"

ENV DEBIAN_FRONTEND=noninteractive

# No libmongoc-dev / libbson-dev: this is a Redis-only build.
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates pkg-config cmake ninja-build build-essential \
        flex bison \
        libsctp-dev libgnutls28-dev libgcrypt-dev libssl-dev libidn11-dev \
        libhiredis-dev libyaml-dev libnghttp2-dev libmicrohttpd-dev \
        libcurl4-gnutls-dev libtins-dev libtalloc-dev meson \
    && rm -rf /var/lib/apt/lists/*

COPY . /opt/open5gs
WORKDIR /opt/open5gs

RUN rm -rf build* \
    && meson setup build -Dmongo=disabled -Dredis=enabled --prefix=/usr \
    && ninja -C build \
    && ninja -C build install

# Example provisioning (against a separate redis container/host):
#   open5gs-dbctl-redis --db-uri redis://redis:6379/ add \
#       --imsi 001010000000001 --key <K> --opc <OPC> --apn internet --sst 1
#
# Point each NF's db_uri at redis://redis:6379/ in its YAML config.
CMD ["bash"]
