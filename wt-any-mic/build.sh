#!/bin/sh
# Cross-compiles the 32-bit dsound.dll proxy with MinGW (Linux, WSL or MSYS2).
# Needs i686-w64-mingw32-g++ (Debian/Ubuntu: apt install g++-mingw-w64-i686).
set -e
cd "$(dirname "$0")"
mkdir -p dist
i686-w64-mingw32-g++ -shared -O2 -Wall -Wextra \
    -o dist/dsound.dll src/dsound_proxy.cpp src/dsound_proxy_mingw.def \
    -static -static-libgcc -static-libstdc++
i686-w64-mingw32-strip dist/dsound.dll
echo "Built dist/dsound.dll"
