#!/usr/bin/env bash

set -euo pipefail

echo "OSTYPE is ${OSTYPE}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/.."

# UPDATE ME IF YOU WANT TO BUILD A DIFF VERSION!
# This script will automatically download it to project root.
Z3_VERSION="4.15.2"
Z3_DIR="$ROOT_DIR/z3-${Z3_VERSION}"

# Clone if not already present
if [ ! -d "$Z3_DIR" ]; then
  git clone --depth=1 --branch "z3-${Z3_VERSION}" https://github.com/Z3Prover/z3 "$Z3_DIR"
fi

cd "$Z3_DIR"
mkdir -p build && cd build

cmake .. -DCMAKE_BUILD_TYPE=Release

# Detect platform and build accordingly
if [[ "$OSTYPE" == "msys" ]]; then
  # On Windows with MSBuild, use /m flag for parallel builds (no -j)
  cmake --build . --config Release -- /m
else
  # On Linux/macOS, use -j flag for parallel builds
  cmake --build . --config Release -j$(nproc)
fi

mkdir -p "$ROOT_DIR/dist/include"
mkdir -p "$ROOT_DIR/dist/lib"

cp -r "$Z3_DIR"/src/api/*.h "$ROOT_DIR/dist/include/"
cp -r "$Z3_DIR"/src/api/c++/z3++.h "$ROOT_DIR/dist/include/"
cp "$Z3_DIR/build/libz3.a" "$ROOT_DIR/dist/lib/"
