#!/bin/sh
# Cross-compiles the 32-bit proxy with MinGW (Linux, WSL or MSYS2).
# Needs i686-w64-mingw32-g++ (Debian/Ubuntu: apt install g++-mingw-w64-i686).
set -e
cd "$(dirname "$0")"
mkdir -p dist
i686-w64-mingw32-g++ -shared -O2 -Wall -Wextra \
    -o dist/xinput1_3.dll src/xinput_proxy.cpp src/xinput_proxy_mingw.def \
    -static -static-libgcc -static-libstdc++
i686-w64-mingw32-strip dist/xinput1_3.dll
cp dist/xinput1_3.dll dist/xinput1_2.dll
cp dist/xinput1_3.dll dist/xinput1_1.dll
echo "Built dist/xinput1_3.dll, xinput1_2.dll, xinput1_1.dll"
