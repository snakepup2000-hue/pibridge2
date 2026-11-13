#!/bin/sh
# The default toolchain on this machine pairs an old ld with a new SDK, which
# fails to link. CommandLineTools ships a matching clang/ld/SDK trio.
set -e
cd "$(dirname "$0")"
CXX=/Library/Developer/CommandLineTools/usr/bin/clang++
SYSROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk
mkdir -p build
"$CXX" -isysroot "$SYSROOT" -std=c++17 -O2 -Wall -Wextra -o build/pibridge src/main.cpp
echo "built build/pibridge"
