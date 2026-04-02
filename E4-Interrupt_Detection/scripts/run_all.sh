#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RESULT_ROOT="${AID_RUN_ALL_RESULT_ROOT:-${ROOT_DIR}/build-linux/unified-benchmarks}"
BASE_BUILD_DIR="${AID_RUN_ALL_BASE_BUILD_DIR:-${ROOT_DIR}/build-linux/unified-base}"
REPEAT_COUNT=100
ITERATIONS=100
INSTR_MODES=(branch once block full)
IPI_VARIANTS=(noipi kthread)
MANIFEST="${RESULT_ROOT}/manifest.tsv"
FIRST_RUN_DONE=0
IPI_DEVICE="/dev/ipi_ctl"
IPI_MODULE_INSTALLED_BY_SCRIPT=0
LLVM_VERSION="18.1.8"
LLVM_TARBALL_NAME="clang+llvm-18.1.8-x86_64-linux-gnu-ubuntu-18.04.tar.xz"
LLVM_TARBALL_URL="https://github.com/llvm/llvm-project/releases/download/llvmorg-18.1.8/${LLVM_TARBALL_NAME}"
LLVM_LOCAL_ROOT="${ROOT_DIR}/llvm-18"
LLVM_BIN_DIR="${LLVM_LOCAL_ROOT}/bin"
LLVM_CMAKE_DIR="${LLVM_LOCAL_ROOT}/lib/cmake/llvm"
PREBUILT_ROOT="${AID_PREBUILT_ROOT:-${ROOT_DIR}/prebuilt}"

usage() {
  cat <<'EOF'
用法：
  ./scripts/run_all.sh

说明：
  - 无需参数，自动运行统一测试矩阵
  - 组合维度：插桩模式 × {无 IPI, kthread IPI}
  - 各程序内部默认循环 100 次
  - 每个组合外层重复 100 次
  - 每次运行结果按目录分类存放，便于后续解析
EOF
}

if [[ $# -gt 0 ]]; then
  usage >&2
  exit 1
fi

cat <<'EOF'
[提示] 本脚本会优先自动检查 LLVM 18 / clang 18 / 构建工具，并在缺失时自动下载官方 LLVM 18 预编译包。
[提示] 首轮运行会自动下载待测试程序源码（mbedtls / wolfssl / libjpeg），后续组合会复用已下载内容。
[提示] 本脚本会尝试运行 noipi 与 kthread IPI 两类测试。
[提示] 若要完成 kthread IPI 测试，请先在目标机器上用 sudo 安装并加载 IPI 内核模块，确保 /dev/ipi_ctl 可用。
[提示] 若未安装模块，脚本仍可完成 noipi 部分，但 kthread 部分无法成功。
EOF

need_cmd() {
  command -v "$1" >/dev/null 2>&1
}

ensure_llvm_ready() {
  if [[ -d "${PREBUILT_ROOT}/branch" && -d "${PREBUILT_ROOT}/once" && -d "${PREBUILT_ROOT}/block" && -d "${PREBUILT_ROOT}/full" ]]; then
    echo "[info] 检测到预构建 mode 目录，将跳过 LLVM 准备。"
    return 0
  fi

  if need_cmd cmake && need_cmd clang-18 && need_cmd opt-18 && need_cmd llvm-dis-18; then
    echo "[info] 已检测到系统 LLVM 18 与构建工具。"
    return 0
  fi

  if [[ -x "${LLVM_BIN_DIR}/clang" && -x "${LLVM_BIN_DIR}/opt" && -x "${LLVM_BIN_DIR}/llvm-dis" && -d "${LLVM_CMAKE_DIR}" ]]; then
    export PATH="${LLVM_BIN_DIR}:${PATH}"
    export LLVM_DIR="${LLVM_CMAKE_DIR}"
    echo "[info] 已检测到本地 LLVM 18 工具链: ${LLVM_LOCAL_ROOT}"
    return 0
  fi

  echo "[warn] 未检测到可用的 LLVM 18，准备下载官方预编译包。" >&2

  if ! need_cmd cmake; then
    echo "[err] 未找到 cmake，请先安装 cmake 后再运行。" >&2
    return 1
  fi

  if ! need_cmd curl; then
    echo "[err] 未找到 curl，无法下载 LLVM 18 预编译包。" >&2
    return 1
  fi

  mkdir -p "${ROOT_DIR}"
  local bundled_tarball_path="${ROOT_DIR}/${LLVM_TARBALL_NAME}"
  local tarball_path="${bundled_tarball_path}"
  local extract_parent="${ROOT_DIR}/.llvm-download"
  local downloaded_tarball=0

  rm -rf "${extract_parent}" "${LLVM_LOCAL_ROOT}"
  mkdir -p "${extract_parent}"

  if [[ ! -f "${bundled_tarball_path}" ]]; then
    tarball_path="${extract_parent}/${LLVM_TARBALL_NAME}"
    downloaded_tarball=1
    echo "[info] 下载 LLVM ${LLVM_VERSION}: ${LLVM_TARBALL_URL}"
    curl -L --fail --retry 3 --retry-delay 2 -o "${tarball_path}" "${LLVM_TARBALL_URL}"
  else
    echo "[info] 使用本地 LLVM tar 包: ${bundled_tarball_path}"
  fi

  echo "[info] 解压 LLVM 到 ${LLVM_LOCAL_ROOT}"
  tar -xf "${tarball_path}" -C "${extract_parent}"

  local extracted_dir
  extracted_dir="$(find "${extract_parent}" -mindepth 1 -maxdepth 1 -type d | head -n 1)"
  if [[ -z "${extracted_dir}" ]]; then
    echo "[err] LLVM 18 预编译包解压失败。" >&2
    return 1
  fi

  mv "${extracted_dir}" "${LLVM_LOCAL_ROOT}"
  if [[ "${downloaded_tarball}" -eq 1 ]]; then
    rm -f "${tarball_path}"
  fi
  rm -rf "${extract_parent}"

  if [[ ! -x "${LLVM_BIN_DIR}/clang" || ! -x "${LLVM_BIN_DIR}/opt" || ! -x "${LLVM_BIN_DIR}/llvm-dis" || ! -d "${LLVM_CMAKE_DIR}" ]]; then
    echo "[err] 下载后的 LLVM 18 不完整：${LLVM_LOCAL_ROOT}" >&2
    return 1
  fi

  export PATH="${LLVM_BIN_DIR}:${PATH}"
  export LLVM_DIR="${LLVM_CMAKE_DIR}"
  echo "[info] LLVM 18 已就绪：${LLVM_LOCAL_ROOT}"
}

ensure_ipi_ready() {
  if [[ -e "${IPI_DEVICE}" ]]; then
    echo "[info] 已检测到 IPI 设备: ${IPI_DEVICE}"
    return 0
  fi

  if [[ "$(id -u)" -eq 0 ]]; then
    echo "[info] 当前已以 root 身份运行，直接安装 IPI 模块..."
    "${ROOT_DIR}/scripts/ipi_module.sh" install
    if [[ -e "${IPI_DEVICE}" ]]; then
      IPI_MODULE_INSTALLED_BY_SCRIPT=1
      echo "[info] IPI 模块安装完成: ${IPI_DEVICE}"
      return 0
    fi
    echo "[warn] root 安装流程执行后仍未检测到 ${IPI_DEVICE}，将仅运行 noipi。" >&2
    return 1
  fi

  if ! command -v sudo >/dev/null 2>&1; then
    echo "[warn] 未找到 sudo，无法自动安装 IPI 模块，将仅运行 noipi。" >&2
    return 1
  fi

  if sudo -n true >/dev/null 2>&1; then
    echo "[info] 检测到免密 sudo，自动安装 IPI 模块..."
    sudo "${ROOT_DIR}/scripts/ipi_module.sh" install
    if [[ -e "${IPI_DEVICE}" ]]; then
      IPI_MODULE_INSTALLED_BY_SCRIPT=1
      echo "[info] IPI 模块安装完成: ${IPI_DEVICE}"
      return 0
    fi
    echo "[warn] sudo 安装流程执行后仍未检测到 ${IPI_DEVICE}，将仅运行 noipi。" >&2
    return 1
  fi

  echo "[warn] 未检测到 ${IPI_DEVICE}，且 sudo 需要交互密码。请直接使用 'sudo ./run_all.sh' 运行；当前将仅运行 noipi。" >&2
  return 1
}

cleanup_ipi_module() {
  if [[ "${IPI_MODULE_INSTALLED_BY_SCRIPT}" -eq 1 ]]; then
    echo "[info] 运行结束，自动卸载 IPI 模块..."
    if ! "${ROOT_DIR}/scripts/ipi_module.sh" uninstall; then
      echo "[warn] 自动卸载 IPI 模块失败，请手动检查。" >&2
    fi
  fi
}

trap cleanup_ipi_module EXIT

mkdir -p "${RESULT_ROOT}" "${BASE_BUILD_DIR}"
printf "variant	instr_mode	repeat_id	status	result_tsv	summary_md	build_dir\n" > "${MANIFEST}"

prepare_shared_artifacts() {
  if [[ -d "${PREBUILT_ROOT}/branch" && -d "${PREBUILT_ROOT}/once" && -d "${PREBUILT_ROOT}/block" && -d "${PREBUILT_ROOT}/full" ]]; then
    return 0
  fi

  if [[ ! -f "${BASE_BUILD_DIR}/AutoInstrumentPass.so" ]]; then
    local llvm_dir="${LLVM_DIR:-/usr/lib/llvm-18/lib/cmake/llvm}"
    if [[ ! -d "${llvm_dir}" && -d "${ROOT_DIR}/llvm-18/lib/cmake/llvm" ]]; then
      llvm_dir="${ROOT_DIR}/llvm-18/lib/cmake/llvm"
    fi
    cmake -S "${ROOT_DIR}" -B "${BASE_BUILD_DIR}" -DLLVM_DIR="${llvm_dir}" >/dev/null
    cmake --build "${BASE_BUILD_DIR}" >/dev/null
  fi

  if [[ ! -f "${BASE_BUILD_DIR}/ipi_sender" ]]; then
    local clang_bin=""
    if command -v clang-18 >/dev/null 2>&1; then
      clang_bin="$(command -v clang-18)"
    elif command -v clang >/dev/null 2>&1; then
      clang_bin="$(command -v clang)"
    fi
    if [[ -n "${clang_bin}" ]]; then
      "${clang_bin}" -O2 -Wall -Wextra \
        "${ROOT_DIR}/tools/ipi/ipi_sender.c" \
        -o "${BASE_BUILD_DIR}/ipi_sender"
    fi
  fi
}

link_shared_artifacts() {
  local rep_dir="$1"
  mkdir -p "${rep_dir}"
  ln -sf "${BASE_BUILD_DIR}/AutoInstrumentPass.so" "${rep_dir}/AutoInstrumentPass.so"
  if [[ -f "${BASE_BUILD_DIR}/ipi_sender" ]]; then
    ln -sf "${BASE_BUILD_DIR}/ipi_sender" "${rep_dir}/ipi_sender"
  fi
  if [[ ! -e "${rep_dir}/AutoInstrumentPass.so" ]]; then
    echo "[err] 缺少共享插件: ${rep_dir}/AutoInstrumentPass.so" >&2
    return 1
  fi
}

run_one() {
  local variant="$1"
  local instr_mode="$2"
  local rep_id="$3"
  local rep_dir="${RESULT_ROOT}/${variant}/${instr_mode}/rep_${rep_id}"
  local -a args=(--skip-build --iterations="${ITERATIONS}" --run-modes=both --random-input)
  local use_prebuilt=0

  if [[ -d "${PREBUILT_ROOT}/${instr_mode}" ]]; then
    use_prebuilt=1
    rm -rf "${rep_dir}"
    mkdir -p "${rep_dir}"
    rsync -a --delete "${PREBUILT_ROOT}/${instr_mode}/" "${rep_dir}/"
  else
    link_shared_artifacts "${rep_dir}"
  fi

  if [[ "${FIRST_RUN_DONE}" -eq 1 ]]; then
    args=(--skip-download "${args[@]}")
  fi

  if [[ "${use_prebuilt}" -eq 1 ]]; then
    args=(--skip-download --skip-build --iterations="${ITERATIONS}" --run-modes=both --random-input)
  fi

  case "${instr_mode}" in
    full)
      args+=(--instrument-full)
      ;;
    *)
      args+=(--secret-instrument="${instr_mode}")
      ;;
  esac

  if [[ "${variant}" == "kthread" ]]; then
    args+=(--with-ipi --ipi-mode=kthread --ipi-duration=auto)
  fi

  local run_status="ok"
  if ! AID_BUILD_DIR="${rep_dir}" "${ROOT_DIR}/scripts/known_cases.sh" "${args[@]}"; then
    run_status="failed"
    echo "[warn] 组合执行失败: variant=${variant} instr_mode=${instr_mode} repeat=${rep_id}" >&2
  fi
  FIRST_RUN_DONE=1

  printf "%s	%s	%s	%s	%s	%s	%s\n" \
    "${variant}" "${instr_mode}" "${rep_id}" "${run_status}" \
    "${rep_dir}/known_cases_results.tsv" \
    "${rep_dir}/known_cases_summary.md" \
    "${rep_dir}" >> "${MANIFEST}"
}

ensure_llvm_ready

prepare_shared_artifacts

if ! ensure_ipi_ready; then
  IPI_VARIANTS=(noipi)
fi

for variant in "${IPI_VARIANTS[@]}"; do
  for instr_mode in "${INSTR_MODES[@]}"; do
    for rep in $(seq -w 1 "${REPEAT_COUNT}"); do
      run_one "${variant}" "${instr_mode}" "${rep}"
    done
  done
done

echo "统一结果目录: ${RESULT_ROOT}"
echo "总索引: ${MANIFEST}"
