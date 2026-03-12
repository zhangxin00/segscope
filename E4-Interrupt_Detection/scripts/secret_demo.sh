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
EXAMPLE_C="${ROOT_DIR}/examples/secret_demo.c"

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

mkdir -p "${BUILD_DIR}"

if [[ ! -f "${EXAMPLE_C}" ]]; then
  cat > "${EXAMPLE_C}" <<'EOF'
#include <stdio.h>

static int foo(int secret) {
  if (secret > 0) {
    return 1;
  }
  return 0;
}

int main(void) {
  int secret = 1;
  return foo(secret);
}
EOF
fi

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

"${CLANG_BIN}" -O0 -g -emit-llvm -c "${EXAMPLE_C}" -o "${BUILD_DIR}/secret_demo.bc"
"${OPT_BIN}" -load-pass-plugin "${PLUGIN}" -passes=intmon-branch \
  -instrument-mode=secret -secret-args=foo:0 \
  "${BUILD_DIR}/secret_demo.bc" -o "${BUILD_DIR}/secret_demo.intmon.bc"
"${LLVMDIS_BIN}" "${BUILD_DIR}/secret_demo.intmon.bc" -o "${BUILD_DIR}/secret_demo.intmon.ll"

echo "Instrumented IR: ${BUILD_DIR}/secret_demo.intmon.ll"
grep -n "__intmon_prepare" "${BUILD_DIR}/secret_demo.intmon.ll" | head -n 5 || true
grep -n "__intmon_check" "${BUILD_DIR}/secret_demo.intmon.ll" | head -n 5 || true
