#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"

mkdir -p "${BUILD_DIR}"
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DLLVM_DIR=/usr/lib/llvm-18/lib/cmake/llvm
cmake --build "${BUILD_DIR}"

echo "Built plugin: ${BUILD_DIR}/AutoInstrumentPass.so"
