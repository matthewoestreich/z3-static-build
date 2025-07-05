#!/usr/bin/env bash

# UPDATE ME IF YOU WANT TO BUILD A DIFF VERSION!
# This script will automatically download it to project root.
Z3_VERSION="4.15.2"

set -euo pipefail

echo "OSTYPE is ${OSTYPE}"

# Get platform
PLATFORM=""
if [[ "$OSTYPE" == "msys" ]]; then
    PLATFORM="win32"
elif [[ "$OSTYPE" == "darwin"* ]]; then
    PLATFORM="darwin"
elif [[ "$OSTYPE" == "linux"* ]]; then
    PLATFORM="linux"
else
    echo "ERROR : UNKNOWN PLATFORM"
    exit 1
fi

# Get architecture
ARCH="$(uname -m)"
if [[ "$ARCH" == *"x86_64"* ]]; then
    ARCH="x64"
elif [[ "$ARCH" == *"arm64"* || "$ARCH" == *"aarch64"* ]]; then
    ARCH="arm64"
else
    echo "ERROR : UNSUPPORTED ARCHITECTURE"
    exit 1
fi

PLAT_ARCH="$PLATFORM-$ARCH"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/.."
Z3_DIR="$ROOT_DIR/z3-${Z3_VERSION}-${PLAT_ARCH}"

# Clone if not already present
if [ ! -d "$Z3_DIR" ]; then
    git clone --depth=1 --branch "z3-${Z3_VERSION}" https://github.com/Z3Prover/z3 "$Z3_DIR"
fi

cd "$Z3_DIR"
mkdir -p build && cd build

cmake .. -DCMAKE_BUILD_TYPE=Release -DZ3_BUILD_LIBZ3_SHARED=OFF

echo "Done configuring!"
echo "Z3_BUILD_LIBZ3_SHARED: $(grep Z3_BUILD_LIBZ3_SHARED CMakeCache.txt)"

# Detect platform and build accordingly
if [[ "$OSTYPE" == "msys" ]]; then
    cmake --build . --verbose --config Release -- -m:5
elif [[ "$OSTYPE" == "darwin"* ]]; then
    # Mac doesn't have 'nproc', use 'sysctl -n hw.physicalcpu' instead
    cmake --build . --verbose --config Release -- -j$(sysctl -n hw.physicalcpu)
else
    # On Linux, use -j$(nproc) flag for parallel builds
    cmake --build . --verbose --config Release -- -j$(nproc)
fi

mkdir -p "$ROOT_DIR/dist/include"
mkdir -p "$ROOT_DIR/dist/lib"

#cp -r "$Z3_DIR"/src/api/*.h "$ROOT_DIR/dist/include/"
#cp -r "$Z3_DIR"/src/api/c++/z3++.h "$ROOT_DIR/dist/include/"
#cp "$Z3_DIR/build/libz3.a" "$ROOT_DIR/dist/lib/"
cp -r "$Z3_DIR/build/" "$ROOT_DIR/dist/"
