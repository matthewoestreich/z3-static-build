#!/usr/bin/env bash

set -euo pipefail

###################################################################################################################
# CHANGE THIS TO BUILD DIFFERENT VERSIONS
###################################################################################################################
Z3_VERSION="4.15.2"
###################################################################################################################

###################################################################################################################
# Setup variables
###################################################################################################################
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/.."
Z3_DIR="$ROOT_DIR/z3-${Z3_VERSION}"
BUILD_DIR="$Z3_DIR/build"

###################################################################################################################
# Clone if not already present
###################################################################################################################
if [ ! -d "$Z3_DIR" ]; then
    echo ""
    echo "[INFO] Z3 version '$Z3_VERSION' not found! Cloning source now..."
    echo ""
    git clone --depth=1 --branch "z3-${Z3_VERSION}" https://github.com/Z3Prover/z3 "$Z3_DIR"
fi

###################################################################################################################
# Get platform
###################################################################################################################
echo "[INFO] Getting platform name"
if [[ "$OSTYPE" == "msys" ]]; then
    PLATFORM="win32"
elif [[ "$OSTYPE" == "darwin"* ]]; then
    PLATFORM="darwin"
elif [[ "$OSTYPE" == "linux"* ]]; then
    PLATFORM="linux"
else
    echo ""
    echo "[ERROR] : Unsupported platform"
    echo ""
    exit 1
fi
###################################################################################################################
# Get architecture
###################################################################################################################
echo "[INFO] Getting architecture"
ARCH="$(uname -m)"
if [[ "$ARCH" == *"x86_64"* ]]; then
    ARCH="x64"
elif [[ "$ARCH" == *"arm64"* || "$ARCH" == *"aarch64"* ]]; then
    ARCH="arm64"
else
    echo ""
    echo "[ERROR] : Unsupported architecture"
    echo ""
    exit 1
fi

###################################################################################################################
# Move into Z3 dir, create build folder, and move into build folder
###################################################################################################################
cd "$Z3_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

###################################################################################################################
# Configure build system
###################################################################################################################
echo ""
echo "[INFO] Attempting to configure build system"
echo ""

cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release -DZ3_BUILD_LIBZ3_SHARED=false ../

###################################################################################################################
# Run 'make'
###################################################################################################################
echo ""
echo "[INFO] Attempting build"
echo ""
if [[ "$PLATFORM" == "win32" ]]; then
    cmake --build . -- -j"$NUMBER_OF_PROCESSORS"
elif [[ "$PLATFORM" == "darwin" ]]; then
    cmake --build . -- -j$(sysctl -n hw.logicalcpu)
elif [[ "$PLATFORM" == "linux" ]]; then
    cmake --build . -- -j$(nproc)
else
    echo ""
    echo "[ERROR] Unrecognized platform encountered while attempting to build"
    echo ""
    exit 1
fi

###################################################################################################################
# Create folder to store built files, aka "archive" path
# Our GitHub Action workflow will pick up files in "ARCHIVE_FOLDER" and archive them
###################################################################################################################
ARCHIVE_DIR="$ROOT_DIR/z3-$Z3_VERSION-$PLATFORM-$ARCH"
ARCHIVE_INCLUDE_DIR="$ARCHIVE_DIR/include"
ARCHIVE_BIN_DIR="$ARCHIVE_DIR/bin"
# Create archive dirs
mkdir -p "$ARCHIVE_INCLUDE_DIR"
mkdir -p "$ARCHIVE_BIN_DIR"

###################################################################################################################
# Copy headers, lib, and license from build to our "archive" path
###################################################################################################################
echo ""
echo "[INFO] Copying build files into archive"
echo ""

# Copy license
echo " - Copying LICENSE"
if [ -f "$Z3_DIR/LICENSE.txt" ]; then
    cp "$Z3_DIR/LICENSE.txt" "$ARCHIVE_DIR"
else
    echo "$Z3_DIR/LICENSE.txt does not exist"
fi

# Copy lib
echo " - Copying libz3"
if [[ "$PLATFORM" == "win32" ]]; then
    if [ -f "$BUILD_DIR/libz3.lib" ]; then
        cp "$BUILD_DIR/libz3.lib" "$ARCHIVE_BIN_DIR"
    else
        echo "$BUILD_DIR/libz3.lib does not exist"
    fi
elif [[ "$PLATFORM" == "darwin" || "$PLATFORM" == "linux" ]]; then
    if [ -f "$BUILD_DIR/libz3.a" ]; then
        cp "$BUILD_DIR/libz3.a" "$ARCHIVE_BIN_DIR"
    else
        echo "$BUILD_DIR/libz3.a does not exist"
    fi
else
    echo ""
    echo "[ERROR] Unrecognized platform encountered while copying libz3"
    echo ""
    exit 1
fi

# Copy headers
# I chose these headers bc that is what the official build provides.
echo " - Copying header files"

headers=(
    "build/src/util/z3_version.h"
    "src/api/z3_v1.h"
    "src/api/z3_spacer.h"
    "src/api/z3_rcf.h"
    "src/api/z3_polynomial.h"
    "src/api/z3_optimization.h"
    "src/api/z3_macros.h"
    "src/api/z3_fpa.h"
    "src/api/z3_fixedpoint.h"
    "src/api/z3_ast_containers.h"
    "src/api/z3_api.h"
    "src/api/z3_algebraic.h"
    "src/api/z3.h"
    "src/api/c++/z3++.h"
)

for header in "${headers[@]}"; do
    full_path="$Z3_DIR/$header"
    if [ -f "$full_path" ]; then
        cp "$full_path" "$ARCHIVE_INCLUDE_DIR"
    else
        echo "$full_path does not exist"
    fi
done

###################################################################################################################
###################################################################################################################
