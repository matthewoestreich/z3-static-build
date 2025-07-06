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
# Move into Z3 dir
###################################################################################################################
cd "$Z3_DIR"

###################################################################################################################
# Run python config script
###################################################################################################################
echo ""
echo "[INFO] Attempting to bootstrap build via 'python'"
echo ""
python scripts/mk_make.py --staticlib --single-threaded

###################################################################################################################
# Move into build folder
###################################################################################################################
cd "$BUILD_DIR"

###################################################################################################################
# Run 'make'
###################################################################################################################
echo ""
echo "[INFO] Attempting to run build via 'make'"
echo ""
if [[ "$OSTYPE" == "msys" ]]; then
    # Windows, use env var $NUMBER_OF_PROCESSORS to get logical number of cores.
    export CXXFLAGS="/FS"
    make -j$NUMBER_OF_PROCESSORS
elif [[ "$OSTYPE" == "darwin"* ]]; then
    # Mac, use 'sysctl -n hw.logicalcpu' to get number of logical cores
    make -j$(sysctl -n hw.logicalcpu)
else
    # Linux, use nproc to get number of logical cores.
    make -j$(nproc)
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
cp "$Z3_DIR/LICENSE.txt" "$ARCHIVE_DIR"
# Copy lib
echo " - Copying libz3.a"
cp "$BUILD_DIR/libz3.a" "$ARCHIVE_BIN_DIR"
# Copy headers
# I chose these headers bc that is what the official build provides.
echo " - Copying header files"
cp "$Z3_DIR/src/util/z3_version.h" "$ARCHIVE_INCLUDE_DIR"
cp "$Z3_DIR/src/api/z3_v1.h" "$ARCHIVE_INCLUDE_DIR"
cp "$Z3_DIR/src/api/z3_spacer.h" "$ARCHIVE_INCLUDE_DIR"
cp "$Z3_DIR/src/api/z3_rcf.h" "$ARCHIVE_INCLUDE_DIR"
cp "$Z3_DIR/src/api/z3_polynomial.h" "$ARCHIVE_INCLUDE_DIR"
cp "$Z3_DIR/src/api/z3_optimization.h" "$ARCHIVE_INCLUDE_DIR"
cp "$Z3_DIR/src/api/z3_macros.h" "$ARCHIVE_INCLUDE_DIR"
cp "$Z3_DIR/src/api/z3_fpa.h" "$ARCHIVE_INCLUDE_DIR"
cp "$Z3_DIR/src/api/z3_fixedpoint.h" "$ARCHIVE_INCLUDE_DIR"
cp "$Z3_DIR/src/api/z3_ast_containers.h" "$ARCHIVE_INCLUDE_DIR"
cp "$Z3_DIR/src/api/z3_api.h" "$ARCHIVE_INCLUDE_DIR"
cp "$Z3_DIR/src/api/z3_algebraic.h" "$ARCHIVE_INCLUDE_DIR"
cp "$Z3_DIR/src/api/z3.h" "$ARCHIVE_INCLUDE_DIR"
cp "$Z3_DIR/src/api/c++/z3++.h" "$ARCHIVE_INCLUDE_DIR"
