#!/bin/sh
# Build a static aarch64 receiver for the Pi, which has no compiler.
#
# Docker Desktop on Apple Silicon runs linux/arm64 natively, so this is a
# native compile inside a container rather than a cross-compile. The only
# header needed is the kernel UAPI's <sound/asound.h>, which linux-libc-dev
# provides; alsa-lib is linked against nothing and never needed at runtime.
set -eu
cd "$(dirname "$0")"

docker run --rm --platform linux/arm64 \
  -v "$PWD":/src -w /src \
  arm64v8/debian:bookworm-slim \
  sh -c '
    set -e
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends gcc libc6-dev linux-libc-dev file >/dev/null
    gcc -O2 -Wall -Wextra -std=gnu11 -static -pthread -o hqdac-aarch64 hqdac.c
    strip hqdac-aarch64
    file hqdac-aarch64
    ls -l hqdac-aarch64
  '
