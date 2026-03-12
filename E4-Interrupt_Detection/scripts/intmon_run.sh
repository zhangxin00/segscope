#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OS_NAME="$(uname -s)"
DEFAULT_BUILD_DIR="${ROOT_DIR}/build"
if [[ "${OS_NAME}" == "Linux" ]]; then
  DEFAULT_BUILD_DIR="${ROOT_DIR}/build-linux"
fi
BUILD_DIR="${AID_BUILD_DIR:-${DEFAULT_BUILD_DIR}}"
PLUGIN=""
INPUT_C=""
RUNTIME_PATH="${ROOT_DIR}/runtime/intmon_runtime.c"
OUT_BIN=""
LLVM_DIR="/usr/lib/llvm-18/lib/cmake/llvm"
SKIP_BUILD=0

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

usage() {
  cat <<EOF
用法：
  $0 [options]

选项：
  -i, --input <file.c>     输入 C 源文件（默认 examples/hello.c）
  -o, --output <file>      输出可执行文件（默认 <build>/\<basename>.intmon）
  -r, --runtime <path>     运行时 stub（.c/.o/.a），默认 runtime/intmon_runtime.c
  -p, --plugin <path>      Pass 插件路径（默认 <build>/AutoInstrumentPass.so）
  -b, --build-dir <dir>    构建目录（默认 build 或 build-linux）
  --llvm-dir <dir>         LLVM CMake 目录（仅 Linux 构建用）
  --no-build               跳过插件构建（要求插件已存在）
  -h, --help               显示帮助

说明：
  branch_id 由 pass 自动生成，无法通过命令行手动指定。
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -i|--input)
      INPUT_C="$2"
      shift 2
      ;;
    -o|--output)
      OUT_BIN="$2"
      shift 2
      ;;
    -r|--runtime)
      RUNTIME_PATH="$2"
      shift 2
      ;;
    -p|--plugin)
      PLUGIN="$2"
      shift 2
      ;;
    -b|--build-dir)
      BUILD_DIR="$2"
      shift 2
      ;;
    --llvm-dir)
      LLVM_DIR="$2"
      shift 2
      ;;
    --no-build)
      SKIP_BUILD=1
      shift 1
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "未知参数: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [[ -z "${INPUT_C}" ]]; then
  INPUT_C="${ROOT_DIR}/examples/hello.c"
fi

if [[ -z "${PLUGIN}" ]]; then
  PLUGIN="${BUILD_DIR}/AutoInstrumentPass.so"
fi

if [[ -z "${OUT_BIN}" ]]; then
  base_name="$(basename "${INPUT_C}")"
  base_name="${base_name%.*}"
  OUT_BIN="${BUILD_DIR}/${base_name}.intmon"
fi

is_elf() {
  local file="$1"
  if [[ ! -f "${file}" ]]; then
    return 1
  fi
  local magic
  magic="$(head -c 4 "${file}" | od -An -t x1 | tr -d ' \n')"
  [[ "${magic}" == "7f454c46" ]]
}

mkdir -p "${BUILD_DIR}"

if [[ "${SKIP_BUILD}" -eq 0 ]]; then
  if [[ "${OS_NAME}" == "Linux" ]]; then
    if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
      cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DLLVM_DIR="${LLVM_DIR}"
    fi
    cmake --build "${BUILD_DIR}"
  else
    if [[ ! -f "${PLUGIN}" ]]; then
      echo "未找到插件 ${PLUGIN}，请先在本机构建或在容器内运行脚本。" >&2
      exit 1
    fi
  fi
else
  if [[ ! -f "${PLUGIN}" ]]; then
    echo "指定跳过构建，但插件不存在：${PLUGIN}" >&2
    exit 1
  fi
fi

if [[ "${OS_NAME}" == "Linux" ]] && ! is_elf "${PLUGIN}"; then
  echo "插件不是 ELF 产物：${PLUGIN}" >&2
  exit 1
fi

"${CLANG_BIN}" -O0 -g -emit-llvm -c "${INPUT_C}" -o "${BUILD_DIR}/intmon_input.bc"
"${OPT_BIN}" -load-pass-plugin "${PLUGIN}" -passes=intmon-branch \
  "${BUILD_DIR}/intmon_input.bc" -o "${BUILD_DIR}/intmon_output.bc"
"${CLANG_BIN}" "${BUILD_DIR}/intmon_output.bc" "${RUNTIME_PATH}" -o "${OUT_BIN}"

echo "Run: ${OUT_BIN}"
"${OUT_BIN}"
