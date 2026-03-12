#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
PLUGIN="${BUILD_DIR}/AutoInstrumentPass.so"
EXAMPLE="${ROOT_DIR}/examples/hello.c"

find_tool() {
  local name="$1"
  shift
  for candidate in "$@"; do
    if command -v "${candidate}" >/dev/null 2>&1; then
      echo "${candidate}"
      return 0
    fi
  done
  echo "未找到可用的 ${name}（尝试: $*)" >&2
  return 1
}

CLANG_BIN="$(find_tool clang clang clang-18 clang-17 clang-16 clang-15)"
OPT_BIN="$(find_tool opt opt opt-18 opt-17 opt-16 opt-15)"
LLVMDIS_BIN="$(find_tool llvm-dis llvm-dis llvm-dis-18 llvm-dis-17 llvm-dis-16 llvm-dis-15)"

if [[ ! -f "${PLUGIN}" ]]; then
  "${ROOT_DIR}/docker/run.sh"
fi

mkdir -p "${BUILD_DIR}"

"${CLANG_BIN}" -O0 -g -emit-llvm -c "${EXAMPLE}" -o "${BUILD_DIR}/hello.bc"
"${OPT_BIN}" -load-pass-plugin "${PLUGIN}" -passes=auto-instrument \
  -aid-target=call -aid-asm=nop "${BUILD_DIR}/hello.bc" -o "${BUILD_DIR}/hello.inst.bc"
"${LLVMDIS_BIN}" "${BUILD_DIR}/hello.inst.bc" -o "${BUILD_DIR}/hello.inst.ll"

echo "Instrumented IR: ${BUILD_DIR}/hello.inst.ll"
grep -n "asm sideeffect" "${BUILD_DIR}/hello.inst.ll" | head -n 5 || true
