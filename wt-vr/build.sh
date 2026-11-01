#!/bin/sh
# Cross-compiles WT:VR's d3d9.dll with MinGW (Linux, WSL or MSYS2). It must be 32-bit, like GHWT.
# Needs i686-w64-mingw32-g++ (Debian/Ubuntu: apt install g++-mingw-w64-i686) and git.
#
# The OpenXR headers (OPENXR_VERSION, a Khronos release tag) are fetched into ./openxr-sdk on the first
# build, or point OPENXR_INCLUDE at an existing OpenXR-SDK "include" folder. The OpenXR loader
# (openxr_loader.dll) isn't built here: it comes from Khronos' release of the same version (the CI
# downloads it; see .github/workflows/build.yml).
#
# WTVR_VERSION (optional) is the version the mod writes at the top of its log.
set -e
cd "$(dirname "$0")"

OPENXR_VERSION=${OPENXR_VERSION:-release-1.1.65}
OPENXR_INCLUDE=${OPENXR_INCLUDE:-openxr-sdk/include}
if [ ! -f "$OPENXR_INCLUDE/openxr/openxr.h" ]; then
    echo "Fetching the OpenXR headers into ./openxr-sdk"
    git clone --depth 1 --branch "$OPENXR_VERSION" https://github.com/KhronosGroup/OpenXR-SDK.git openxr-sdk
    OPENXR_INCLUDE=openxr-sdk/include
fi

mkdir -p dist
i686-w64-mingw32-g++ -std=c++17 -O2 -Wall -shared -fno-threadsafe-statics \
    -DWTVR_VERSION="\"${WTVR_VERSION:-dev}\"" -I"$OPENXR_INCLUDE" \
    -o dist/d3d9.dll src/*.cpp src/d3d9.def \
    -static -static-libgcc -static-libstdc++ -Wl,--kill-at -Wl,--enable-stdcall-fixup -s
echo "Built dist/d3d9.dll"
