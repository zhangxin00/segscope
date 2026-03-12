#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OS_NAME="$(uname -s)"
DEFAULT_BUILD_DIR="${ROOT_DIR}/build"
if [[ "${OS_NAME}" == "Linux" ]]; then
  DEFAULT_BUILD_DIR="${ROOT_DIR}/build-linux"
fi
BUILD_DIR="${AID_BUILD_DIR:-${DEFAULT_BUILD_DIR}}"
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

is_elf() {
  local file="$1"
  if [[ ! -f "${file}" ]]; then
    return 1
  fi
  local magic
  magic="$(head -c 4 "${file}" | od -An -t x1 | tr -d ' \n')"
  [[ "${magic}" == "7f454c46" ]]
}

if [[ "${OS_NAME}" == "Linux" ]]; then
  if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
    cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DLLVM_DIR=/usr/lib/llvm-18/lib/cmake/llvm
  fi
  cmake --build "${BUILD_DIR}"
else
  if [[ ! -f "${PLUGIN}" ]]; then
    echo "未找到插件 ${PLUGIN}，请先在本机构建或在容器内运行脚本。" >&2
    exit 1
  fi
fi

mkdir -p "${BUILD_DIR}"

"${CLANG_BIN}" -O0 -g -emit-llvm -c "${EXAMPLE}" -o "${BUILD_DIR}/hello.bc"
"${OPT_BIN}" -load-pass-plugin "${PLUGIN}" -passes=intmon-branch \
  "${BUILD_DIR}/hello.bc" -o "${BUILD_DIR}/hello.intmon.bc"
"${LLVMDIS_BIN}" "${BUILD_DIR}/hello.intmon.bc" -o "${BUILD_DIR}/hello.intmon.ll"

echo "Instrumented IR: ${BUILD_DIR}/hello.intmon.ll"
grep -n "__intmon_prepare" "${BUILD_DIR}/hello.intmon.ll" | head -n 5 || true
grep -n "__intmon_check" "${BUILD_DIR}/hello.intmon.ll" | head -n 5 || true
