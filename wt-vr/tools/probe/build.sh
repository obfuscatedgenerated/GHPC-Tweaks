#!/bin/sh
# Cross-compiles the probe's d3d9.dll with MinGW (32-bit). PASSTHROUGH=1 builds the variant that hooks
# nothing (to check a proxy loads at all).
set -e
cd "$(dirname "$0")"
mkdir -p dist
if [ "${PASSTHROUGH:-0}" = "1" ]; then DEFINES="-DPROBE_PASSTHROUGH"; else DEFINES=""; fi
i686-w64-mingw32-g++ -std=c++17 -O2 -Wall -shared $DEFINES \
    -o dist/d3d9.dll probe.cpp d3d9.def \
    -static -static-libgcc -static-libstdc++ -Wl,--kill-at -Wl,--enable-stdcall-fixup -s
echo "Built tools/probe/dist/d3d9.dll"
