#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
THIRD_DIR="${ROOT_DIR}/third_party"
BUILD_DIR_DEFAULT="${ROOT_DIR}/build-linux"
BUILD_DIR="${AID_BUILD_DIR:-${BUILD_DIR_DEFAULT}}"
PLUGIN="${BUILD_DIR}/AutoInstrumentPass.so"
DISTFILES_DIR="${AID_DISTFILES_DIR:-${ROOT_DIR}/distfiles}"
LLVM_DIR_DEFAULT="/usr/lib/llvm-18/lib/cmake/llvm"
# 自动检测 LLVM 18：优先环境变量 > 系统安装 > 项目内 llvm-18 目录
if [[ -n "${LLVM_DIR:-}" && -d "${LLVM_DIR:-}" ]]; then
  : # 环境变量已设置且有效
elif [[ -d "${LLVM_DIR_DEFAULT}" ]]; then
  LLVM_DIR="${LLVM_DIR_DEFAULT}"
elif [[ -d "${ROOT_DIR}/llvm-18/lib/cmake/llvm" ]]; then
  LLVM_DIR="${ROOT_DIR}/llvm-18/lib/cmake/llvm"
else
  LLVM_DIR="${LLVM_DIR:-${LLVM_DIR_DEFAULT}}"
  echo "[警告] LLVM_DIR=${LLVM_DIR} 不存在，cmake 可能使用系统 LLVM" >&2
fi
ORIG_ARGS=("$@")

usage() {
  cat <<EOF
用法：
  $0 [--skip-download] [--skip-build] [--f2-at=succ|join] [--secret-instrument=once|branch|block|both] [--use-gs] [--inline-gs] [--run-modes=api|core|both] [--llvm-dir=<dir>] [--syscall-funcs=<list>] [--syscall-func-prefixes=<list>] [--with-ipi ...]

说明：
  - 默认会下载/解压四个已知测试用例源码（mbedtls 2.6.1/3.6.1, wolfssl 5.7.2, jpeg 9f）
  - 默认会构建本工程插件（build-linux/AutoInstrumentPass.so）
  - 需要网络访问时请确保已允许（首次运行）
  - --f2-at 用于控制 __intmon_check 插入位置（succ 或 join）
  - --secret-instrument 控制 secret 分支插桩范围（once: 仅分支级；branch: 分支到 join 之间逐指令；block: 分支到 join 之间按“系统调用分段”插桩；both: branch/once 依次运行）
  - --syscall-funcs 视为 syscall 边界的库函数名（逗号分隔，覆盖默认列表）
  - --syscall-func-prefixes 视为 syscall 边界的库函数名前缀（逗号分隔）
  - --use-gs 使用 GS 模式运行（仅适用于真实 x86_64 Linux 环境；容器/虚拟化可能失败）
  - --inline-gs GS 模式下将 prepare/check 内联为汇编指令（需配合 --use-gs）
  - --run-modes 控制 MbedTLS/WolfSSL 的运行模式（api/core/both，默认 both）
  - --llvm-dir 用于指定 LLVM_DIR（默认 /usr/lib/llvm-18/lib/cmake/llvm）
  - --with-ipi 启用 IPI 中断打断测试（需要内核模块 /dev/ipi_ctl）
  - --ipi-mode IPI 发送模式（user|kthread|timer|apic，默认 user）
  - --time-source 计时来源（monotonic|tsc，默认 monotonic）
  - --instrument-full 对每条指令插桩（跳过 PHI/调试/生命周期/终结指令）
  - --iterations 重复执行次数（默认 100）
  - --ipi-rate IPI 发送频率（次/秒，默认 100000）
  - --ipi-duration IPI 发送时长（秒，默认 2；auto 表示覆盖 B 运行周期）
  - --ipi-warmup-ms 发送端启动后等待时间（毫秒，默认 50）
  - --random-input 启用随机输入（支持的用例，如 Huffman/Kyber）
  - --ipi-target-cpu 目标 CPU（B 程序绑定核心）
  - --ipi-sender-cpu 发送端绑定核心（需与 target 不同）
  - --ipi-device IPI 设备路径（默认 /dev/ipi_ctl）
  - --ipi-wait 等待 IPI 回调完成（默认异步）

环境变量：
  AID_OPT_MEM_LIMIT_KB  opt 进程虚拟内存上限（KB，默认 40GB=41943040），防止 OOM Killer
EOF
}

SKIP_DOWNLOAD=0
SKIP_BUILD=0
F2_AT="succ"
SECRET_INSTR="branch"
USE_GS=0
INLINE_GS=0
RUN_MODES="both"
WITH_IPI=0
IPI_MODE="user"
INSTR_EVERY=0
ITERATIONS="${AID_KNOWN_ITERS:-100}"
TIME_SOURCE="${INTMON_TIME_SOURCE:-monotonic}"
SYSCALL_FUNCS=""
SYSCALL_FUNC_PREFIXES=""
IPI_RATE=100000
IPI_DURATION=2
RANDOM_INPUT="${AID_RANDOM_INPUT:-0}"
IPI_WARMUP_MS=50
IPI_DEVICE="/dev/ipi_ctl"
IPI_TARGET_CPU=""
IPI_SENDER_CPU=""
IPI_WAIT=0
IPI_SENDER_BIN=""
TASKSET_BIN=""
TASKSET_PREFIX=()
IPI_DURATION_IS_AUTO=0
OPT_MEM_LIMIT_KB="${AID_OPT_MEM_LIMIT_KB:-41943040}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --skip-download)
      SKIP_DOWNLOAD=1
      shift
      ;;
    --skip-build)
      SKIP_BUILD=1
      shift
      ;;
    --f2-at=*)
      F2_AT="${1#*=}"
      shift
      ;;
    --f2-at)
      F2_AT="$2"
      shift 2
      ;;
    --secret-instrument=*)
      SECRET_INSTR="${1#*=}"
      shift
      ;;
    --secret-instrument)
      SECRET_INSTR="$2"
      shift 2
      ;;
    --use-gs)
      USE_GS=1
      shift
      ;;
    --inline-gs)
      INLINE_GS=1
      shift
      ;;
    --llvm-dir=*)
      LLVM_DIR="${1#*=}"
      shift
      ;;
    --llvm-dir)
      LLVM_DIR="$2"
      shift 2
      ;;
    --run-modes=*)
      RUN_MODES="${1#*=}"
      shift
      ;;
    --run-modes)
      RUN_MODES="$2"
      shift 2
      ;;
    --with-ipi)
      WITH_IPI=1
      shift
      ;;
    --ipi-mode=*)
      IPI_MODE="${1#*=}"
      shift
      ;;
    --ipi-mode)
      IPI_MODE="$2"
      shift 2
      ;;
    --time-source=*)
      TIME_SOURCE="${1#*=}"
      shift
      ;;
    --time-source)
      TIME_SOURCE="$2"
      shift 2
      ;;
    --instrument-full)
      INSTR_EVERY=1
      shift
      ;;
    --syscall-funcs=*)
      SYSCALL_FUNCS="${1#*=}"
      shift
      ;;
    --syscall-funcs)
      SYSCALL_FUNCS="$2"
      shift 2
      ;;
    --syscall-func-prefixes=*)
      SYSCALL_FUNC_PREFIXES="${1#*=}"
      shift
      ;;
    --syscall-func-prefixes)
      SYSCALL_FUNC_PREFIXES="$2"
      shift 2
      ;;
    --iterations=*)
      ITERATIONS="${1#*=}"
      shift
      ;;
    --iterations)
      ITERATIONS="$2"
      shift 2
      ;;
    --ipi-rate=*)
      IPI_RATE="${1#*=}"
      shift
      ;;
    --ipi-rate)
      IPI_RATE="$2"
      shift 2
      ;;
    --ipi-duration=*)
      IPI_DURATION="${1#*=}"
      shift
      ;;
    --ipi-duration)
      IPI_DURATION="$2"
      shift 2
      ;;
    --ipi-warmup-ms=*)
      IPI_WARMUP_MS="${1#*=}"
      shift
      ;;
    --ipi-warmup-ms)
      IPI_WARMUP_MS="$2"
      shift 2
      ;;
    --ipi-target-cpu=*)
      IPI_TARGET_CPU="${1#*=}"
      shift
      ;;
    --ipi-target-cpu)
      IPI_TARGET_CPU="$2"
      shift 2
      ;;
    --ipi-sender-cpu=*)
      IPI_SENDER_CPU="${1#*=}"
      shift
      ;;
    --ipi-sender-cpu)
      IPI_SENDER_CPU="$2"
      shift 2
      ;;
    --ipi-device=*)
      IPI_DEVICE="${1#*=}"
      shift
      ;;
    --ipi-device)
      IPI_DEVICE="$2"
      shift 2
      ;;
    --ipi-wait)
      IPI_WAIT=1
      shift
      ;;
    --random-input)
      RANDOM_INPUT=1
      shift
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

if ! [[ "${ITERATIONS}" =~ ^[0-9]+$ ]]; then
  echo "无效 --iterations: ${ITERATIONS}（需为正整数）" >&2
  exit 1
fi
if [[ "${ITERATIONS}" -lt 1 ]]; then
  ITERATIONS=1
fi
export AID_KNOWN_ITERS="${ITERATIONS}"

if [[ "${RANDOM_INPUT}" -eq 1 ]]; then
  export AID_RANDOM_INPUT=1
fi

case "${RUN_MODES}" in
  both)
    RUN_MODES="api core"
    ;;
  api|core)
    ;;
  *)
    echo "无效 --run-modes: ${RUN_MODES}（仅支持 api/core/both）" >&2
    exit 1
    ;;
esac

case "${SECRET_INSTR}" in
  once|branch|block|both)
    ;;
  *)
    echo "无效 --secret-instrument: ${SECRET_INSTR}（仅支持 once/branch/block/both）" >&2
    exit 1
    ;;
esac

case "${IPI_MODE}" in
  user|kthread|timer|apic)
    ;;
  *)
    echo "无效 --ipi-mode: ${IPI_MODE}（仅支持 user|kthread|timer|apic）" >&2
    exit 1
    ;;
esac

case "${TIME_SOURCE}" in
  monotonic|mono)
    TIME_SOURCE="monotonic"
    ;;
  tsc)
    ;;
  *)
    echo "无效 --time-source: ${TIME_SOURCE}（仅支持 monotonic|tsc）" >&2
    exit 1
    ;;
esac

if [[ "${INSTR_EVERY}" -eq 1 && "${SECRET_INSTR}" == "both" ]]; then
  echo "[warn] 已启用 --instrument-full，忽略 --secret-instrument=both" >&2
  SECRET_INSTR="branch"
fi

if [[ "${SECRET_INSTR}" == "both" ]]; then
  REINVOKE_ARGS=()
  skip_next=0
  for arg in "${ORIG_ARGS[@]}"; do
    if [[ "${skip_next}" -eq 1 ]]; then
      skip_next=0
      continue
    fi
    case "${arg}" in
      --secret-instrument=*)
        ;;
      --secret-instrument)
        skip_next=1
        ;;
      *)
        REINVOKE_ARGS+=("${arg}")
        ;;
    esac
  done
  echo "[info] secret-instrument=both -> 依次运行 branch/once"
  AID_OUTPUT_SUFFIX=".branch" "$0" "${REINVOKE_ARGS[@]}" --secret-instrument=branch
  AID_OUTPUT_SUFFIX=".once" "$0" "${REINVOKE_ARGS[@]}" --secret-instrument=once
  exit 0
fi

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

extract_kv() {
  local line="$1"
  local key="$2"
  awk -v k="${key}" '{for(i=1;i<=NF;i++){split($i,a,"="); if(a[1]==k){print a[2]}}}' <<< "${line}"
}

extract_first_kv() {
  local line="$1"
  shift
  for key in "$@"; do
    local val
    val="$(extract_kv "${line}" "${key}")"
    if [[ -n "${val}" ]]; then
      echo "${val}"
      return 0
    fi
  done
  echo ""
}

file_size() {
  local path="$1"
  if [[ -z "${path}" || ! -f "${path}" ]]; then
    echo "-"
    return 0
  fi
  if stat -c%s "${path}" >/dev/null 2>&1; then
    stat -c%s "${path}"
    return 0
  fi
  if stat -f%z "${path}" >/dev/null 2>&1; then
    stat -f%z "${path}"
    return 0
  fi
  wc -c < "${path}" | awk '{print $1}'
}

cpu_stat_snapshot() {
  awk '/^cpu[0-9]+ / {id=substr($1,4); idle=$5+$6; total=0; for(i=2;i<=NF;i++){total+=$i} print id, idle, total}' /proc/stat
}

pick_idle_cpus() {
  local snap1 snap2
  snap1="$(cpu_stat_snapshot)"
  sleep 1
  snap2="$(cpu_stat_snapshot)"

  declare -A idle1 total1
  while read -r id idle total; do
    idle1["${id}"]="${idle}"
    total1["${id}"]="${total}"
  done <<< "${snap1}"

  local -a candidates=()
  while read -r id idle total; do
    local prev_idle prev_total
    prev_idle="${idle1[${id}]:-0}"
    prev_total="${total1[${id}]:-0}"
    local d_idle=$((idle - prev_idle))
    local d_total=$((total - prev_total))
    if [[ "${d_total}" -le 0 ]]; then
      continue
    fi
    local permil=$((d_idle * 1000 / d_total))
    candidates+=("${permil} ${id}")
  done <<< "${snap2}"

  if [[ "${#candidates[@]}" -lt 2 ]]; then
    return 1
  fi

  local sorted cpu1 cpu2
  sorted="$(printf "%s\n" "${candidates[@]}" | sort -nr)"
  cpu1="$(echo "${sorted}" | awk 'NR==1{print $2}')"
  cpu2="$(echo "${sorted}" | awk 'NR==2{print $2}')"
  if [[ -z "${cpu1}" || -z "${cpu2}" ]]; then
    return 1
  fi
  echo "${cpu1} ${cpu2}"
}

select_ipi_cpus() {
  local total_cpus
  total_cpus="$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc || echo 1)"
  if [[ -z "${total_cpus}" || "${total_cpus}" -lt 1 ]]; then
    total_cpus=1
  fi
  if [[ "${IPI_MODE}" == "timer" ]]; then
    if [[ -n "${IPI_SENDER_CPU}" && "${IPI_SENDER_CPU}" != "${IPI_TARGET_CPU}" ]]; then
      echo "[warn] timer 模式忽略 --ipi-sender-cpu" >&2
    fi
    if [[ -z "${IPI_TARGET_CPU}" ]]; then
      local picked
      picked="$(pick_idle_cpus || true)"
      if [[ -n "${picked}" ]]; then
        read -r IPI_TARGET_CPU _ <<< "${picked}"
      else
        IPI_TARGET_CPU=0
      fi
    fi
    IPI_SENDER_CPU=""
    echo "[info] IPI 绑定: target_cpu=${IPI_TARGET_CPU}, sender_cpu=${IPI_TARGET_CPU}" >&2
    return 0
  fi
  if [[ -n "${IPI_TARGET_CPU}" && -n "${IPI_SENDER_CPU}" ]]; then
    if [[ "${IPI_TARGET_CPU}" == "${IPI_SENDER_CPU}" ]]; then
      echo "[err] IPI 目标 CPU 与发送端 CPU 不能相同" >&2
      exit 1
    fi
    return 0
  fi
  if [[ "${total_cpus}" -lt 2 ]]; then
    echo "[err] CPU 核心数不足（至少需要 2 个核心）" >&2
    exit 1
  fi
  local picked
  picked="$(pick_idle_cpus || true)"
  if [[ -z "${picked}" ]]; then
    picked="0 1"
  fi
  local cpu1 cpu2
  read -r cpu1 cpu2 <<< "${picked}"
  if [[ -z "${IPI_TARGET_CPU}" ]]; then
    IPI_TARGET_CPU="${cpu1}"
  fi
  if [[ -z "${IPI_SENDER_CPU}" ]]; then
    if [[ "${IPI_TARGET_CPU}" == "${cpu2}" ]]; then
      IPI_SENDER_CPU="${cpu1}"
    else
      IPI_SENDER_CPU="${cpu2}"
    fi
  fi
  if [[ "${IPI_TARGET_CPU}" == "${IPI_SENDER_CPU}" ]]; then
    for ((i=0; i<total_cpus; i++)); do
      if [[ "${i}" != "${IPI_TARGET_CPU}" ]]; then
        IPI_SENDER_CPU="${i}"
        break
      fi
    done
  fi
  if [[ "${IPI_TARGET_CPU}" == "${IPI_SENDER_CPU}" ]]; then
    echo "[err] 无法选择不同的 IPI 核心" >&2
    exit 1
  fi
  echo "[info] IPI 绑定: target_cpu=${IPI_TARGET_CPU}, sender_cpu=${IPI_SENDER_CPU}" >&2
}

build_ipi_sender() {
  local src="${ROOT_DIR}/tools/ipi/ipi_sender.c"
  local out="${BUILD_DIR}/ipi_sender"
  if [[ ! -f "${src}" ]]; then
    echo "[err] 未找到 ipi_sender 源码: ${src}" >&2
    exit 1
  fi
  if [[ ! -f "${out}" || "${src}" -nt "${out}" ]]; then
    ${CLANG_BIN} -O2 -Wall -Wextra "${src}" -o "${out}"
  fi
  IPI_SENDER_BIN="${out}"
}

setup_ipi() {
  if [[ ! -e "${IPI_DEVICE}" ]]; then
    echo "[err] 未找到 IPI 设备: ${IPI_DEVICE}" >&2
    echo "[err] 需要加载内核模块并授权设备访问，例如：" >&2
    echo "  sudo insmod ${ROOT_DIR}/kernel/ipi_kmod/ipi_kmod.ko" >&2
    echo "  sudo chmod 666 ${IPI_DEVICE}" >&2
    exit 1
  fi
  TASKSET_BIN="$(find_tool taskset taskset)"
  select_ipi_cpus
  TASKSET_PREFIX=("${TASKSET_BIN}" -c "${IPI_TARGET_CPU}")
  build_ipi_sender
}

run_with_taskset() {
  if [[ "${#TASKSET_PREFIX[@]}" -eq 0 ]]; then
    "$@"
  else
    "${TASKSET_PREFIX[@]}" "$@"
  fi
}

run_with_ipi() {
  local ipi_log="$1"
  shift
  if [[ "${WITH_IPI}" -ne 1 ]]; then
    run_with_taskset "$@"
    return
  fi
  if [[ "${IPI_MODE}" == "timer" && ( -z "${IPI_RATE}" || "${IPI_RATE}" == "0" ) ]]; then
    echo "[err] timer 模式必须指定正的 --ipi-rate" >&2
    return 1
  fi
  local sender_cmd=("${IPI_SENDER_BIN}" -t "${IPI_TARGET_CPU}" \
    -r "${IPI_RATE}" -D "${IPI_DURATION}" -d "${IPI_DEVICE}" -m "${IPI_MODE}")
  if [[ -n "${IPI_SENDER_CPU}" ]]; then
    sender_cmd+=(-s "${IPI_SENDER_CPU}")
  fi
  if [[ "${IPI_WAIT}" -eq 1 ]]; then
    sender_cmd+=(--wait)
  fi

  local sender_desc="mode=${IPI_MODE} target=${IPI_TARGET_CPU} sender=${IPI_SENDER_CPU:-auto} rate=${IPI_RATE} duration=${IPI_DURATION} wait=${IPI_WAIT} device=${IPI_DEVICE}"
  local sender_cmdline
  sender_cmdline="$(printf "%q " "${sender_cmd[@]}")"
  (
    echo "[ipi_sender_cmd] ${sender_desc}"
    echo "[ipi_sender_cmdline] ${sender_cmdline}"
    exec "${sender_cmd[@]}"
  ) > "${ipi_log}" 2>&1 &
  local sender_pid=$!

  if [[ "${IPI_WARMUP_MS}" -gt 0 ]]; then
    local sleep_s
    sleep_s="$(awk -v ms="${IPI_WARMUP_MS}" 'BEGIN{printf "%.3f", ms/1000.0}')"
    sleep "${sleep_s}"
  fi

  run_with_taskset "$@"
  if [[ "${IPI_DURATION_IS_AUTO}" -eq 1 ]]; then
    kill -TERM "${sender_pid}" >/dev/null 2>&1 || true
    for _ in {1..20}; do
      if ! kill -0 "${sender_pid}" >/dev/null 2>&1; then
        break
      fi
      sleep 0.05
    done
    if kill -0 "${sender_pid}" >/dev/null 2>&1; then
      kill -KILL "${sender_pid}" >/dev/null 2>&1 || true
    fi
  fi
  wait "${sender_pid}" || true
}

run_with_selected_condition() {
  local ipi_log="$1"
  shift

  local saved_duration="${IPI_DURATION}"
  local saved_auto="${IPI_DURATION_IS_AUTO}"

  if [[ "${WITH_IPI}" -eq 1 && "${IPI_DURATION}" == "auto" ]]; then
    IPI_DURATION_IS_AUTO=1
    IPI_DURATION=0
  else
    IPI_DURATION_IS_AUTO=0
  fi

  run_with_ipi "${ipi_log}" "$@"
  local rc=$?

  IPI_DURATION="${saved_duration}"
  IPI_DURATION_IS_AUTO="${saved_auto}"
  return "${rc}"
}

LLVM_BIN=""
if [[ -n "${LLVM_DIR}" && -d "${LLVM_DIR}" ]]; then
  LLVM_PREFIX="$(cd "${LLVM_DIR}/../../.." && pwd 2>/dev/null || true)"
  if [[ -n "${LLVM_PREFIX}" && -x "${LLVM_PREFIX}/bin/opt" ]]; then
    LLVM_BIN="${LLVM_PREFIX}/bin"
  fi
fi

if [[ -n "${LLVM_BIN}" ]]; then
  CLANG_BIN="${LLVM_BIN}/clang"
  OPT_BIN="${LLVM_BIN}/opt"
  LLVMDIS_BIN="${LLVM_BIN}/llvm-dis"
else
  CLANG_BIN="$(find_tool clang clang-18 clang-17 clang-16 clang-15 clang)"
  OPT_BIN="$(find_tool opt opt-18 opt-17 opt-16 opt-15 opt)"
  LLVMDIS_BIN="$(find_tool llvm-dis llvm-dis-18 llvm-dis-17 llvm-dis-16 llvm-dis-15 llvm-dis)"
fi

mkdir -p "${THIRD_DIR}" "${BUILD_DIR}"

extract_tarball_if_needed() {
  local src_path="$1"
  local dest_dir="$2"
  local strip_components="${3:-1}"
  if [[ -d "${dest_dir}" ]]; then
    return 0
  fi
  if [[ ! -f "${src_path}" ]]; then
    return 1
  fi
  mkdir -p "${dest_dir}"
  if ! tar -xf "${src_path}" -C "${dest_dir}" --strip-components="${strip_components}"; then
    rm -rf "${dest_dir}"
    return 1
  fi
  return 0
}

if [[ "${SKIP_DOWNLOAD}" -eq 0 ]]; then
  if [[ ! -d "${THIRD_DIR}/mbedtls-2.6.1" ]]; then
    if ! extract_tarball_if_needed "${DISTFILES_DIR}/mbedtls-2.6.1.tar.gz" "${THIRD_DIR}/mbedtls-2.6.1" 1; then
      git clone --depth 1 --branch mbedtls-2.6.1 https://github.com/Mbed-TLS/mbedtls.git "${THIRD_DIR}/mbedtls-2.6.1"
    fi
  fi
  if [[ ! -d "${THIRD_DIR}/mbedtls-3.6.1" ]]; then
    if ! extract_tarball_if_needed "${DISTFILES_DIR}/mbedtls-3.6.1.tar.gz" "${THIRD_DIR}/mbedtls-3.6.1" 1; then
      git clone --depth 1 --branch mbedtls-3.6.1 https://github.com/Mbed-TLS/mbedtls.git "${THIRD_DIR}/mbedtls-3.6.1"
    fi
  fi
  if [[ ! -d "${THIRD_DIR}/wolfssl-5.7.2" ]]; then
    if ! extract_tarball_if_needed "${DISTFILES_DIR}/wolfssl-5.7.2.tar.gz" "${THIRD_DIR}/wolfssl-5.7.2" 1; then
      git clone --depth 1 --branch v5.7.2-stable https://github.com/wolfSSL/wolfssl.git "${THIRD_DIR}/wolfssl-5.7.2"
    fi
  fi
  if [[ ! -d "${THIRD_DIR}/jpeg-9f" ]]; then
    if ! extract_tarball_if_needed "${DISTFILES_DIR}/jpegsrc.v9f.tar.gz" "${THIRD_DIR}/jpeg-9f" 1; then
      curl -L -o "${THIRD_DIR}/jpegsrc.v9f.tar.gz" https://www.ijg.org/files/jpegsrc.v9f.tar.gz
      tar -xf "${THIRD_DIR}/jpegsrc.v9f.tar.gz" -C "${THIRD_DIR}"
    fi
  fi
fi

if [[ "${SKIP_BUILD}" -eq 0 ]]; then
  # 检查已有 CMakeCache 是否指向正确的 LLVM
  if [[ -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
    cached_llvm="$(grep '^LLVM_DIR:PATH=' "${BUILD_DIR}/CMakeCache.txt" 2>/dev/null | cut -d= -f2- || true)"
    if [[ -n "${cached_llvm}" && -d "${LLVM_DIR}" && "${cached_llvm}" != "${LLVM_DIR}" ]]; then
      echo "[警告] CMakeCache 中 LLVM_DIR=${cached_llvm} 与当前 ${LLVM_DIR} 不一致，重新运行 cmake" >&2
      rm -f "${BUILD_DIR}/CMakeCache.txt"
    fi
  fi
  if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
    cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DLLVM_DIR="${LLVM_DIR}"
  fi
  cmake --build "${BUILD_DIR}" >/dev/null
fi

if [[ "${WITH_IPI}" -eq 1 ]]; then
  setup_ipi
fi

OUTPUT_SUFFIX="${AID_OUTPUT_SUFFIX:-}"
RESULTS_TSV="${BUILD_DIR}/known_cases_results${OUTPUT_SUFFIX}.tsv"
SUMMARY_MD="${BUILD_DIR}/known_cases_summary${OUTPUT_SUFFIX}.md"
RAW_TSV="$(mktemp "${BUILD_DIR}/known_cases_results.raw${OUTPUT_SUFFIX}.XXXX.tsv")"
trap 'rm -f "${RAW_TSV}"' EXIT
echo -e "case\tlibrary\tversion\talgorithm\ttarget_branch\tinstrument_mode\tipi_mode\trun_mode\tgs_mode\titerations\tsecret_branches\tprepare_call\tprepare_cnt\tcheck_call\tcheck_cnt\tir_path\tbin_size_base\tbin_size_inst\tbin_size_delta\trun_rc\trun_rc_base\tlog_path\tcounts_path\tlog_base_path\tlog_base_func_path\tipi_log_path\ttime_src\ttime_unit\ttime_total\ttime_func\ttime_func_pct\ttime_base_total\ttime_base_func\ttime_base_func_pct\ttime_overhead_pct" > "${RAW_TSV}"

emit_run_metadata() {
  local name="$1"
  local library="$2"
  local version="$3"
  local algorithm="$4"
  local target_branch="$5"
  local instrument_mode_value="$6"
  local ipi_mode_value="$7"
  local run_mode_value="$8"
  local run_role="$9"
  local gs_mode="${10}"
  local iterations="${11}"
  printf "[known_case_meta] case=%s library=%s version=%s algorithm=%s target_branch=%s instrument_mode=%s ipi_mode=%s run_mode=%s run_role=%s gs_mode=%s iterations=%s\n" \
    "${name}" "${library}" "${version}" "${algorithm}" "${target_branch}" "${instrument_mode_value}" "${ipi_mode_value}" "${run_mode_value}" "${run_role}" "${gs_mode}" "${iterations}"
}

run_case() {
  local name="$1"
  local library="$2"
  local version="$3"
  local algorithm="$4"
  local src="$5"
  local outdir="$6"
  local includes="$7"
  local cflags="$8"
  local lines="$9"
  local main_src="${10}"
  local extra_srcs="${11:-}"
  local run_modes="${12:-}"
  local sec_flags="-ffunction-sections -fdata-sections"
  local gs_mode="tls"
  local instrument_mode_value="${SECRET_INSTR}"
  local ipi_mode_value="none"
  local target_branch
  local extra_link_flags=()
  local execution_only=0
  local ir_path="-"
  local execution_only=0
  local ir_path="-"
  if [[ "${USE_GS}" -eq 1 ]]; then
    gs_mode="gs"
  fi
  if [[ "${INSTR_EVERY}" -eq 1 ]]; then
    instrument_mode_value="full"
  fi
  if [[ "${WITH_IPI}" -eq 1 ]]; then
    ipi_mode_value="${IPI_MODE}"
  fi
  target_branch="$(printf "%b" "${lines}" | tr '\n' ',')"
  target_branch="${target_branch%,}"
  if [[ "${library}" == "wolfssl" && "${algorithm}" == "DH" ]]; then
    extra_link_flags+=(-lm)
  fi

  mkdir -p "${outdir}"
  rm -f "${outdir}"/*.o
  printf "\\n=== %s ===\\n" "${name}"

  if [[ "${SKIP_BUILD}" -eq 1 && -f "${outdir}/case.out" && -f "${outdir}/case_base.out" ]]; then
    execution_only=1
  fi

  if [[ "${execution_only}" -eq 0 ]]; then
    # build bitcode
    ${CLANG_BIN} -O0 -g ${cflags} ${includes} -emit-llvm -c "${src}" -o "${outdir}/input.bc"
    printf "%b\\n" "${lines}" > "${outdir}/secret_lines.txt"
  fi

  local instr_every_flag=()
  local inline_gs_flag=()
  local syscall_flags=()
  if [[ "${INSTR_EVERY}" -eq 1 ]]; then
    instr_every_flag+=("-instrument-full")
  fi
  if [[ "${INLINE_GS}" -eq 1 ]]; then
    if [[ "${USE_GS}" -ne 1 ]]; then
      echo "[warn] 已启用 --inline-gs 但未启用 --use-gs，可能无效或导致检测异常" >&2
    fi
    inline_gs_flag+=("-inline-gs")
  fi
  if [[ -n "${SYSCALL_FUNCS}" ]]; then
    syscall_flags+=("-syscall-funcs=${SYSCALL_FUNCS}")
  fi
  if [[ -n "${SYSCALL_FUNC_PREFIXES}" ]]; then
    syscall_flags+=("-syscall-func-prefixes=${SYSCALL_FUNC_PREFIXES}")
  fi
  if [[ "${execution_only}" -eq 0 ]]; then
  local opt_rc=0
  (ulimit -v "${OPT_MEM_LIMIT_KB}" 2>/dev/null || true
   ${OPT_BIN} -load-pass-plugin "${PLUGIN}" -passes=intmon-branch \
    -instrument-mode=secret -secret-lines="${outdir}/secret_lines.txt" \
    -secret-instrument="${SECRET_INSTR}" \
    -f2-at="${F2_AT}" \
    "${inline_gs_flag[@]}" \
    "${syscall_flags[@]}" \
    "${instr_every_flag[@]}" \
    "${outdir}/input.bc" -o "${outdir}/instrumented.bc") || opt_rc=$?
  if [[ "${opt_rc}" -ne 0 ]]; then
    echo "[error] opt 插桩失败（rc=${opt_rc}），跳过 ${name}" >&2
    printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" \
      "${name}" "${library}" "${version}" "${algorithm}" "${target_branch}" "${instrument_mode_value}" "${ipi_mode_value}" "-" "${gs_mode}" "${ITERATIONS}" "OPT_FAIL(${opt_rc})" \
      "-" "-" "-" "-" "-" "-" "-" "-" "-" "-" "-" "-" "-" "-" "-" "-" "-" "-" "-" "-" "-" "-" "-" "-" >> "${RAW_TSV}"
    return 0
  fi

  ${LLVMDIS_BIN} "${outdir}/instrumented.bc" -o "${outdir}/instrumented.ll"
  fi
  if [[ -f "${outdir}/instrumented.ll" ]]; then
    ir_path="${outdir}/instrumented.ll"
  fi
  local bin_size_base="-"
  local bin_size_inst="-"
  local bin_size_delta="-"

  local f1c
  local f1cnt
  local f2c
  local f2cnt
  f1c=$(grep -F -c "call void @__intmon_prepare(" "${outdir}/instrumented.ll" || true)
  f1cnt=$(grep -F -c "call void @__intmon_prepare_cnt(" "${outdir}/instrumented.ll" || true)
  f2c=$(grep -F -c "call void @__intmon_check(" "${outdir}/instrumented.ll" || true)
  f2cnt=$(grep -F -c "call void @__intmon_check_cnt(" "${outdir}/instrumented.ll" || true)
  if [[ "${INLINE_GS}" -eq 1 ]]; then
    f1c="${f1cnt}"
    f2c="${f2cnt}"
  fi
  if [[ -z "${main_src}" ]]; then
    printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" \
      "${name}" "${library}" "${version}" "${algorithm}" "${target_branch}" "${instrument_mode_value}" "${ipi_mode_value}" "-" "${gs_mode}" "${ITERATIONS}" "see-log" "${f1c}" "${f1cnt}" "${f2c}" "${f2cnt}" \
      "${ir_path}" "${bin_size_base}" "${bin_size_inst}" "${bin_size_delta}" \
      "-" "-" "-" "-" "-" "-" "-" "-" "-" "-" "-" "-" "-" "-" "-" "-" "-" "-" "-" "-" >> "${RAW_TSV}"
  fi

  if [[ -f "${outdir}/instrumented.ll" ]]; then
    grep -n "__intmon_prepare" "${outdir}/instrumented.ll" | head -n 5 || true
    grep -n "__intmon_check" "${outdir}/instrumented.ll" | head -n 5 || true
  fi

  if [[ -n "${main_src}" ]]; then
    if [[ "${execution_only}" -eq 0 ]]; then
      local extra_objs=()
      if [[ -n "${extra_srcs}" ]]; then
        IFS=' ' read -r -a extra_list <<< "${extra_srcs}"
        for extra_src in "${extra_list[@]}"; do
          local obj_name
          obj_name="${outdir}/$(basename "${extra_src}").o"
          ${CLANG_BIN} -O0 -g ${sec_flags} ${cflags} ${includes} -c "${extra_src}" -o "${obj_name}"
          extra_objs+=("${obj_name}")
        done
      fi
      ${CLANG_BIN} -O0 -g ${sec_flags} ${cflags} ${includes} -c "${main_src}" -o "${outdir}/case_main.o"
      local runtime_flags=()
      if [[ "${USE_GS}" -eq 1 ]]; then
        runtime_flags+=(-DINTMON_USE_GS)
      fi
      if [[ "${TIME_SOURCE}" == "tsc" ]]; then
        runtime_flags+=(-DINTMON_TIME_TSC)
      fi
      ${CLANG_BIN} -O0 -g ${sec_flags} "${runtime_flags[@]}" \
        -c "${ROOT_DIR}/runtime/intmon_runtime.c" -o "${outdir}/intmon_runtime.o"
      ${CLANG_BIN} -O0 -g "${outdir}/instrumented.bc" \
        "${outdir}/case_main.o" "${outdir}/intmon_runtime.o" "${extra_objs[@]}" \
        "${extra_link_flags[@]}" -Wl,--gc-sections -o "${outdir}/case.out"
      ${CLANG_BIN} -O0 -g "${outdir}/input.bc" \
        "${outdir}/case_main.o" "${outdir}/intmon_runtime.o" "${extra_objs[@]}" \
        "${extra_link_flags[@]}" -Wl,--gc-sections -o "${outdir}/case_base.out"
      local opt2_rc=0
      (ulimit -v "${OPT_MEM_LIMIT_KB}" 2>/dev/null || true
       ${OPT_BIN} -load-pass-plugin "${PLUGIN}" -passes=intmon-branch \
        -instrument-mode=none -func-instrument=instrumented \
        -secret-lines="${outdir}/secret_lines.txt" \
        -secret-instrument="${SECRET_INSTR}" \
        "${inline_gs_flag[@]}" \
        "${outdir}/input.bc" -o "${outdir}/func_only.bc") || opt2_rc=$?
      if [[ "${opt2_rc}" -eq 0 ]]; then
        ${CLANG_BIN} -O0 -g "${outdir}/func_only.bc" \
          "${outdir}/case_main.o" "${outdir}/intmon_runtime.o" "${extra_objs[@]}" \
          "${extra_link_flags[@]}" -Wl,--gc-sections -o "${outdir}/case_func.out"
      else
        echo "[warn] func-only opt 失败（rc=${opt2_rc}），跳过 func 基线" >&2
      fi
    fi

    bin_size_base="$(file_size "${outdir}/case_base.out")"
    bin_size_inst="$(file_size "${outdir}/case.out")"
    if [[ "${bin_size_base}" =~ ^[0-9]+$ && "${bin_size_inst}" =~ ^[0-9]+$ ]]; then
      bin_size_delta=$((bin_size_inst - bin_size_base))
    else
      bin_size_delta="-"
    fi
    if [[ -n "${run_modes}" ]]; then
      for mode in ${run_modes}; do
        echo "[run ${mode}]"
        local log_file="${outdir}/run_${mode}.log"
        local counts_file="${outdir}/counts_${mode}.txt"
        local log_base="${outdir}/run_${mode}_base.log"
        local log_base_func="${outdir}/run_${mode}_base_func.log"
        local ipi_log="${outdir}/ipi_${mode}.log"
        local total_time_base="-"
        local func_time_base="-"
        local func_pct_base="-"
        local rc_base=0
        local ipi_log_base="${outdir}/ipi_${mode}_base.log"
        ({ emit_run_metadata "${name}" "${library}" "${version}" "${algorithm}" "${target_branch}" "${instrument_mode_value}" "${ipi_mode_value}" "${mode}" "baseline_total" "${gs_mode}" "${ITERATIONS}"; run_with_selected_condition "${ipi_log_base}" "${outdir}/case_base.out" "${mode}"; } 2>&1 | tee "${log_base}") || rc_base=${PIPESTATUS[0]}
        local time_line_base
        time_line_base="$(grep "\\[intmon\\] time" "${log_base}" | tail -n 1 || true)"
        if [[ -n "${time_line_base}" ]]; then
          total_time_base="$(extract_first_kv "${time_line_base}" total_ns total_ticks total)"
        fi
        local rc_base_func=0
        local ipi_log_base_func="${outdir}/ipi_${mode}_base_func.log"
        if [[ -f "${outdir}/case_func.out" ]]; then
          ({ emit_run_metadata "${name}" "${library}" "${version}" "${algorithm}" "${target_branch}" "${instrument_mode_value}" "${ipi_mode_value}" "${mode}" "baseline_func" "${gs_mode}" "${ITERATIONS}"; run_with_selected_condition "${ipi_log_base_func}" "${outdir}/case_func.out" "${mode}"; } 2>&1 | tee "${log_base_func}") || rc_base_func=${PIPESTATUS[0]}
          local time_line_base_func
          time_line_base_func="$(grep "\\[intmon\\] time" "${log_base_func}" | tail -n 1 || true)"
          if [[ -n "${time_line_base_func}" ]]; then
            func_time_base="$(extract_first_kv "${time_line_base_func}" func_ns func_ticks func)"
            func_pct_base="$(extract_kv "${time_line_base_func}" func_pct)"
          fi
        fi
        [[ -z "${total_time_base}" ]] && total_time_base="-"
        [[ -z "${func_time_base}" ]] && func_time_base="-"
        [[ -z "${func_pct_base}" ]] && func_pct_base="-"

        ({ emit_run_metadata "${name}" "${library}" "${version}" "${algorithm}" "${target_branch}" "${instrument_mode_value}" "${ipi_mode_value}" "${mode}" "instrumented" "${gs_mode}" "${ITERATIONS}"; run_with_selected_condition "${ipi_log}" "${outdir}/case.out" "${mode}"; } 2>&1 | tee "${log_file}") || rc=${PIPESTATUS[0]}
        grep "\\[intmon\\] COUNT" "${log_file}" > "${counts_file}" || true
        local time_line
        local time_src="-"
        local time_unit="-"
        local total_time="-"
        local func_time="-"
        local func_pct="-"
        time_line="$(grep "\\[intmon\\] time" "${log_file}" | tail -n 1 || true)"
        if [[ -n "${time_line}" ]]; then
          time_src="$(extract_kv "${time_line}" source)"
          time_unit="$(extract_kv "${time_line}" unit)"
          total_time="$(extract_first_kv "${time_line}" total_ns total_ticks total)"
          func_time="$(extract_first_kv "${time_line}" func_ns func_ticks func)"
          func_pct="$(extract_kv "${time_line}" func_pct)"
          [[ -z "${time_src}" ]] && time_src="monotonic"
          [[ -z "${time_unit}" ]] && time_unit="ns"
        fi
        [[ -z "${total_time}" ]] && total_time="-"
        [[ -z "${func_time}" ]] && func_time="-"
        [[ -z "${func_pct}" ]] && func_pct="-"

        local overhead_pct="-"
        if [[ "${total_time}" =~ ^[0-9]+$ && "${total_time_base}" =~ ^[0-9]+$ && "${total_time_base}" -gt 0 ]]; then
          overhead_pct="$(awk -v i="${total_time}" -v b="${total_time_base}" 'BEGIN{printf("%.2f", (i-b)*100.0/b)}')"
        fi

        printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" \
          "${name}" "${library}" "${version}" "${algorithm}" "${target_branch}" "${instrument_mode_value}" "${ipi_mode_value}" "${mode}" "${gs_mode}" "${ITERATIONS}" "see-log" "${f1c}" "${f1cnt}" "${f2c}" "${f2cnt}" \
          "${outdir}/instrumented.ll" "${bin_size_base}" "${bin_size_inst}" "${bin_size_delta}" \
          "${rc:-0}" "${rc_base:-0}" "${log_file}" "${counts_file}" "${log_base}" "${log_base_func}" "${ipi_log}" \
          "${time_src}" "${time_unit}" "${total_time}" "${func_time}" "${func_pct}" "${total_time_base}" "${func_time_base}" "${func_pct_base}" \
          "${overhead_pct}" >> "${RAW_TSV}"
        unset rc
        unset rc_base
        unset rc_base_func
      done
    else
      local log_file="${outdir}/run_default.log"
      local counts_file="${outdir}/counts_default.txt"
      local log_base="${outdir}/run_default_base.log"
      local log_base_func="${outdir}/run_default_base_func.log"
      local ipi_log="${outdir}/ipi_default.log"
      local total_time_base="-"
      local func_time_base="-"
      local func_pct_base="-"
      local rc_base=0
      local ipi_log_base="${outdir}/ipi_default_base.log"
      ({ emit_run_metadata "${name}" "${library}" "${version}" "${algorithm}" "${target_branch}" "${instrument_mode_value}" "${ipi_mode_value}" "default" "baseline_total" "${gs_mode}" "${ITERATIONS}"; run_with_selected_condition "${ipi_log_base}" "${outdir}/case_base.out"; } 2>&1 | tee "${log_base}") || rc_base=${PIPESTATUS[0]}
      local time_line_base
      time_line_base="$(grep "\\[intmon\\] time" "${log_base}" | tail -n 1 || true)"
      if [[ -n "${time_line_base}" ]]; then
        total_time_base="$(extract_first_kv "${time_line_base}" total_ns total_ticks total)"
      fi
      local rc_base_func=0
      local ipi_log_base_func="${outdir}/ipi_default_base_func.log"
      if [[ -f "${outdir}/case_func.out" ]]; then
        ({ emit_run_metadata "${name}" "${library}" "${version}" "${algorithm}" "${target_branch}" "${instrument_mode_value}" "${ipi_mode_value}" "default" "baseline_func" "${gs_mode}" "${ITERATIONS}"; run_with_selected_condition "${ipi_log_base_func}" "${outdir}/case_func.out"; } 2>&1 | tee "${log_base_func}") || rc_base_func=${PIPESTATUS[0]}
        local time_line_base_func
        time_line_base_func="$(grep "\\[intmon\\] time" "${log_base_func}" | tail -n 1 || true)"
        if [[ -n "${time_line_base_func}" ]]; then
          func_time_base="$(extract_first_kv "${time_line_base_func}" func_ns func_ticks func)"
          func_pct_base="$(extract_kv "${time_line_base_func}" func_pct)"
        fi
      fi
      [[ -z "${total_time_base}" ]] && total_time_base="-"
      [[ -z "${func_time_base}" ]] && func_time_base="-"
      [[ -z "${func_pct_base}" ]] && func_pct_base="-"

      ({ emit_run_metadata "${name}" "${library}" "${version}" "${algorithm}" "${target_branch}" "${instrument_mode_value}" "${ipi_mode_value}" "default" "instrumented" "${gs_mode}" "${ITERATIONS}"; run_with_selected_condition "${ipi_log}" "${outdir}/case.out"; } 2>&1 | tee "${log_file}") || rc=${PIPESTATUS[0]}
      grep "\\[intmon\\] COUNT" "${log_file}" > "${counts_file}" || true
      local time_line
      local time_src="-"
      local time_unit="-"
      local total_time="-"
      local func_time="-"
      local func_pct="-"
      time_line="$(grep "\\[intmon\\] time" "${log_file}" | tail -n 1 || true)"
      if [[ -n "${time_line}" ]]; then
        time_src="$(extract_kv "${time_line}" source)"
        time_unit="$(extract_kv "${time_line}" unit)"
        total_time="$(extract_first_kv "${time_line}" total_ns total_ticks total)"
        func_time="$(extract_first_kv "${time_line}" func_ns func_ticks func)"
        func_pct="$(extract_kv "${time_line}" func_pct)"
        [[ -z "${time_src}" ]] && time_src="monotonic"
        [[ -z "${time_unit}" ]] && time_unit="ns"
      fi
      [[ -z "${total_time}" ]] && total_time="-"
      [[ -z "${func_time}" ]] && func_time="-"
      [[ -z "${func_pct}" ]] && func_pct="-"

      local overhead_pct="-"
      if [[ "${total_time}" =~ ^[0-9]+$ && "${total_time_base}" =~ ^[0-9]+$ && "${total_time_base}" -gt 0 ]]; then
        overhead_pct="$(awk -v i="${total_time}" -v b="${total_time_base}" 'BEGIN{printf("%.2f", (i-b)*100.0/b)}')"
      fi

      printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" \
        "${name}" "${library}" "${version}" "${algorithm}" "${target_branch}" "${instrument_mode_value}" "${ipi_mode_value}" "default" "${gs_mode}" "${ITERATIONS}" "see-log" "${f1c}" "${f1cnt}" "${f2c}" "${f2cnt}" \
        "${outdir}/instrumented.ll" "${bin_size_base}" "${bin_size_inst}" "${bin_size_delta}" \
        "${rc:-0}" "${rc_base:-0}" "${log_file}" "${counts_file}" "${log_base}" "${log_base_func}" "${ipi_log}" \
        "${time_src}" "${time_unit}" "${total_time}" "${func_time}" "${func_pct}" "${total_time_base}" "${func_time_base}" "${func_pct_base}" \
        "${overhead_pct}" >> "${RAW_TSV}"
      unset rc
      unset rc_base
      unset rc_base_func
    fi
  fi
}

run_case "MbedTLS 2.6.1 bignum.c" \
  "mbedtls" "2.6.1" "RSA" \
  "${THIRD_DIR}/mbedtls-2.6.1/library/bignum.c" \
  "${BUILD_DIR}/mbedtls-2.6.1" \
  "-I${THIRD_DIR}/mbedtls-2.6.1/include -I${THIRD_DIR}/mbedtls-2.6.1/library" \
  "" \
  "bignum.c:1013\nbignum.c:1044\nbignum.c:1581" \
  "${ROOT_DIR}/examples/known_cases/mbedtls_2_6_1_main.c" \
  "${THIRD_DIR}/mbedtls-2.6.1/library/rsa.c" \
  "${RUN_MODES}"

run_case "MbedTLS 2.6.1 bignum.c (DHM)" \
  "mbedtls" "2.6.1" "DH" \
  "${THIRD_DIR}/mbedtls-2.6.1/library/bignum.c" \
  "${BUILD_DIR}/mbedtls-2.6.1-dhm" \
  "-I${THIRD_DIR}/mbedtls-2.6.1/include -I${THIRD_DIR}/mbedtls-2.6.1/library" \
  "" \
  "bignum.c:1013\nbignum.c:1044\nbignum.c:1581" \
  "${ROOT_DIR}/examples/known_cases/mbedtls_2_6_1_dhm_main.c" \
  "${THIRD_DIR}/mbedtls-2.6.1/library/dhm.c ${THIRD_DIR}/mbedtls-2.6.1/library/asn1parse.c ${THIRD_DIR}/mbedtls-2.6.1/library/pem.c" \
  "${RUN_MODES}"

run_case "MbedTLS 2.6.1 bignum.c (ECDSA)" \
  "mbedtls" "2.6.1" "ECDSA" \
  "${THIRD_DIR}/mbedtls-2.6.1/library/bignum.c" \
  "${BUILD_DIR}/mbedtls-2.6.1-ecdsa" \
  "-I${THIRD_DIR}/mbedtls-2.6.1/include -I${THIRD_DIR}/mbedtls-2.6.1/library" \
  "" \
  "bignum.c:1013\nbignum.c:1044\nbignum.c:1581" \
  "${ROOT_DIR}/examples/known_cases/mbedtls_2_6_1_ecdsa_main.c" \
  "${THIRD_DIR}/mbedtls-2.6.1/library/ecdsa.c ${THIRD_DIR}/mbedtls-2.6.1/library/ecp.c ${THIRD_DIR}/mbedtls-2.6.1/library/ecp_curves.c ${THIRD_DIR}/mbedtls-2.6.1/library/asn1parse.c ${THIRD_DIR}/mbedtls-2.6.1/library/asn1write.c" \
  "${RUN_MODES}"

run_case "MbedTLS 3.6.1 bignum.c" \
  "mbedtls" "3.6.1" "RSA" \
  "${THIRD_DIR}/mbedtls-3.6.1/library/bignum.c" \
  "${BUILD_DIR}/mbedtls-3.6.1" \
  "-I${THIRD_DIR}/mbedtls-3.6.1/include -I${THIRD_DIR}/mbedtls-3.6.1/library" \
  "" \
  "bignum.c:1823\nbignum.c:1959" \
  "${ROOT_DIR}/examples/known_cases/mbedtls_3_6_1_main.c" \
  "${THIRD_DIR}/mbedtls-3.6.1/library/bignum_core.c ${THIRD_DIR}/mbedtls-3.6.1/library/bignum_mod.c ${THIRD_DIR}/mbedtls-3.6.1/library/bignum_mod_raw.c ${THIRD_DIR}/mbedtls-3.6.1/library/platform_util.c ${THIRD_DIR}/mbedtls-3.6.1/library/constant_time.c ${THIRD_DIR}/mbedtls-3.6.1/library/rsa.c ${THIRD_DIR}/mbedtls-3.6.1/library/rsa_alt_helpers.c" \
  "${RUN_MODES}"

run_case "MbedTLS 3.6.1 bignum.c (DHM)" \
  "mbedtls" "3.6.1" "DH" \
  "${THIRD_DIR}/mbedtls-3.6.1/library/bignum.c" \
  "${BUILD_DIR}/mbedtls-3.6.1-dhm" \
  "-I${THIRD_DIR}/mbedtls-3.6.1/include -I${THIRD_DIR}/mbedtls-3.6.1/library" \
  "" \
  "bignum.c:1823\nbignum.c:1959" \
  "${ROOT_DIR}/examples/known_cases/mbedtls_3_6_1_dhm_main.c" \
  "${THIRD_DIR}/mbedtls-3.6.1/library/dhm.c ${THIRD_DIR}/mbedtls-3.6.1/library/asn1parse.c ${THIRD_DIR}/mbedtls-3.6.1/library/pem.c ${THIRD_DIR}/mbedtls-3.6.1/library/bignum_core.c ${THIRD_DIR}/mbedtls-3.6.1/library/bignum_mod.c ${THIRD_DIR}/mbedtls-3.6.1/library/bignum_mod_raw.c ${THIRD_DIR}/mbedtls-3.6.1/library/platform_util.c ${THIRD_DIR}/mbedtls-3.6.1/library/constant_time.c" \
  "${RUN_MODES}"

run_case "MbedTLS 3.6.1 bignum.c (ECDSA)" \
  "mbedtls" "3.6.1" "ECDSA" \
  "${THIRD_DIR}/mbedtls-3.6.1/library/bignum.c" \
  "${BUILD_DIR}/mbedtls-3.6.1-ecdsa" \
  "-I${THIRD_DIR}/mbedtls-3.6.1/include -I${THIRD_DIR}/mbedtls-3.6.1/library" \
  "" \
  "bignum.c:1823\nbignum.c:1959" \
  "${ROOT_DIR}/examples/known_cases/mbedtls_3_6_1_ecdsa_main.c" \
  "${THIRD_DIR}/mbedtls-3.6.1/library/ecdsa.c ${THIRD_DIR}/mbedtls-3.6.1/library/ecp.c ${THIRD_DIR}/mbedtls-3.6.1/library/ecp_curves.c ${THIRD_DIR}/mbedtls-3.6.1/library/bignum_core.c ${THIRD_DIR}/mbedtls-3.6.1/library/bignum_mod.c ${THIRD_DIR}/mbedtls-3.6.1/library/bignum_mod_raw.c ${THIRD_DIR}/mbedtls-3.6.1/library/platform_util.c ${THIRD_DIR}/mbedtls-3.6.1/library/constant_time.c" \
  "${RUN_MODES}"

run_case "WolfSSL 5.7.2 integer.c" \
  "wolfssl" "5.7.2" "RSA" \
  "${THIRD_DIR}/wolfssl-5.7.2/wolfcrypt/src/integer.c" \
  "${BUILD_DIR}/wolfssl-5.7.2" \
  "-I${THIRD_DIR}/wolfssl-5.7.2 -I${THIRD_DIR}/wolfssl-5.7.2/wolfssl" \
  "-DUSE_INTEGER_HEAP_MATH -DWC_RSA_NO_PADDING" \
  "integer.c:1329" \
  "${ROOT_DIR}/examples/known_cases/wolfssl_5_7_2_main.c" \
  "${THIRD_DIR}/wolfssl-5.7.2/wolfcrypt/src/memory.c ${THIRD_DIR}/wolfssl-5.7.2/wolfcrypt/src/rsa.c ${ROOT_DIR}/examples/known_cases/wolfssl_rng_stub.c" \
  "${RUN_MODES}"

run_case "WolfSSL 5.7.2 integer.c (DH)" \
  "wolfssl" "5.7.2" "DH" \
  "${THIRD_DIR}/wolfssl-5.7.2/wolfcrypt/src/integer.c" \
  "${BUILD_DIR}/wolfssl-5.7.2-dh" \
  "-I${THIRD_DIR}/wolfssl-5.7.2 -I${THIRD_DIR}/wolfssl-5.7.2/wolfssl" \
  "-DUSE_INTEGER_HEAP_MATH" \
  "integer.c:1329" \
  "${ROOT_DIR}/examples/known_cases/wolfssl_5_7_2_dh_main.c" \
  "${THIRD_DIR}/wolfssl-5.7.2/wolfcrypt/src/memory.c ${THIRD_DIR}/wolfssl-5.7.2/wolfcrypt/src/dh.c ${ROOT_DIR}/examples/known_cases/wolfssl_rng_stub.c" \
  "${RUN_MODES}"

run_case "WolfSSL 5.7.2 integer.c (ECDSA)" \
  "wolfssl" "5.7.2" "ECDSA" \
  "${THIRD_DIR}/wolfssl-5.7.2/wolfcrypt/src/integer.c" \
  "${BUILD_DIR}/wolfssl-5.7.2-ecdsa" \
  "-I${THIRD_DIR}/wolfssl-5.7.2 -I${THIRD_DIR}/wolfssl-5.7.2/wolfssl" \
  "-DUSE_INTEGER_HEAP_MATH -DHAVE_ECC -DHAVE_ECC_SIGN -DHAVE_ECC_VERIFY -DHAVE_ECC_KEY_IMPORT" \
  "integer.c:1329" \
  "${ROOT_DIR}/examples/known_cases/wolfssl_5_7_2_ecdsa_main.c" \
  "${THIRD_DIR}/wolfssl-5.7.2/wolfcrypt/src/memory.c ${THIRD_DIR}/wolfssl-5.7.2/wolfcrypt/src/wolfmath.c ${THIRD_DIR}/wolfssl-5.7.2/wolfcrypt/src/asn.c ${THIRD_DIR}/wolfssl-5.7.2/wolfcrypt/src/ecc.c ${ROOT_DIR}/examples/known_cases/wolfssl_rng_stub.c" \
  "${RUN_MODES}"

# libjpeg needs jconfig.h
if [[ ! -f "${THIRD_DIR}/jpeg-9f/jconfig.h" ]]; then
  cp "${THIRD_DIR}/jpeg-9f/jconfig.txt" "${THIRD_DIR}/jpeg-9f/jconfig.h"
fi

  run_case "Libjpeg 9f jidctint.c" \
  "libjpeg" "9f" "JPEG_IDCT" \
  "${THIRD_DIR}/jpeg-9f/jidctint.c" \
  "${BUILD_DIR}/jpeg-9f" \
  "-I${THIRD_DIR}/jpeg-9f" \
  "" \
  "jidctint.c:208\njidctint.c:325" \
  "${ROOT_DIR}/examples/known_cases/jpeg_9f_main.c" \
  "${THIRD_DIR}/jpeg-9f/jdapimin.c ${THIRD_DIR}/jpeg-9f/jdapistd.c ${THIRD_DIR}/jpeg-9f/jdatasrc.c ${THIRD_DIR}/jpeg-9f/jdmaster.c ${THIRD_DIR}/jpeg-9f/jdinput.c ${THIRD_DIR}/jpeg-9f/jdmarker.c ${THIRD_DIR}/jpeg-9f/jdhuff.c ${THIRD_DIR}/jpeg-9f/jdmainct.c ${THIRD_DIR}/jpeg-9f/jdcoefct.c ${THIRD_DIR}/jpeg-9f/jdpostct.c ${THIRD_DIR}/jpeg-9f/jdsample.c ${THIRD_DIR}/jpeg-9f/jddctmgr.c ${THIRD_DIR}/jpeg-9f/jquant1.c ${THIRD_DIR}/jpeg-9f/jquant2.c ${THIRD_DIR}/jpeg-9f/jdcolor.c ${THIRD_DIR}/jpeg-9f/jcomapi.c ${THIRD_DIR}/jpeg-9f/jerror.c ${THIRD_DIR}/jpeg-9f/jutils.c ${THIRD_DIR}/jpeg-9f/jmemmgr.c ${THIRD_DIR}/jpeg-9f/jmemnobs.c ${THIRD_DIR}/jpeg-9f/jdmerge.c ${THIRD_DIR}/jpeg-9f/jdarith.c ${THIRD_DIR}/jpeg-9f/jaricom.c ${THIRD_DIR}/jpeg-9f/jidctfst.c ${THIRD_DIR}/jpeg-9f/jidctflt.c" \
  "${RUN_MODES}"



echo
echo "结果汇总: ${RESULTS_TSV}"

build_final_tsv() {
  local raw_tsv="$1"
  local out_tsv="$2"
  echo -e "case\tlibrary\tversion\talgorithm\ttarget_branch\tinstrument_mode\tipi_mode\tmode\tgs\titerations\trc\trc_base\tprepare\tprepare_total\tcheck\tcheck_total\tinterrupt_detect\tipi_count\tipi_handled\tipi_rate\tipi_elapsed\tipi_target\tipi_sender\tbin_size_base\tbin_size_inst\tbin_size_delta\ttime_src\ttime_unit\ttotal_time\tbase_total_time\toverhead_pct\tfunc_time\tbase_func_time\tfunc_overhead_pct\tlog\tcounts\tlog_base\tlog_base_func\tipi_log" > "${out_tsv}"
  while IFS=$'\t' read -r case library version algorithm target_branch instrument_mode ipi_mode mode gs iterations secret_branches prepare_call prepare_cnt \
      check_call check_cnt ir_path bin_size_base bin_size_inst bin_size_delta \
      run_rc run_rc_base log_path counts_path log_base_path log_base_func_path \
      ipi_log_path time_src time_unit total_time func_time func_pct total_time_base func_time_base func_pct_base \
      overhead_pct; do
    if [[ "${case}" == "case" ]]; then
      continue
    fi
    prepare_total="-"
    check_total="-"
    interrupt_detect="-"
    if [[ -n "${counts_path}" && -f "${counts_path}" ]]; then
      prepare_total=$(awk '/COUNT prepare/ {for(i=1;i<=NF;i++){split($i,a,"="); if(a[1]=="total"){sum+=a[2]}}} END{print sum+0}' "${counts_path}")
      check_total=$(awk '/COUNT check/ {for(i=1;i<=NF;i++){split($i,a,"="); if(a[1]=="total"){sum+=a[2]}}} END{print sum+0}' "${counts_path}")
    fi
    if [[ -n "${log_path}" && -f "${log_path}" ]]; then
      interrupt_detect="$(awk '/\[intmon\] interrupt_detect/ {for(i=1;i<=NF;i++){split($i,a,"="); if(a[1]=="total"){v=a[2]}}} END{if(v!="") print v}' "${log_path}")"
      if [[ -z "${interrupt_detect}" ]]; then
        interrupt_detect=$(grep -c "\\[intmon\\] interrupt detect" "${log_path}" || true)
      fi
    fi
    local ipi_count="-" ipi_handled="-" ipi_rate="-" ipi_elapsed="-" ipi_target="-" ipi_sender="-"
    if [[ -n "${ipi_log_path}" && -f "${ipi_log_path}" ]]; then
      ipi_count="$(awk 'index($0,"[ipi_sender]")>0 {for(i=1;i<=NF;i++){split($i,a,"="); if(a[1]=="count"){print a[2]}}}' "${ipi_log_path}" | tail -n 1)"
      ipi_handled="$(awk 'index($0,"[ipi_sender]")>0 {for(i=1;i<=NF;i++){split($i,a,"="); if(a[1]=="handled"){print a[2]}}}' "${ipi_log_path}" | tail -n 1)"
      ipi_rate="$(awk 'index($0,"[ipi_sender]")>0 {for(i=1;i<=NF;i++){split($i,a,"="); if(a[1]=="rate"){print a[2]}}}' "${ipi_log_path}" | tail -n 1)"
      ipi_elapsed="$(awk 'index($0,"[ipi_sender]")>0 {for(i=1;i<=NF;i++){split($i,a,"="); if(a[1]=="elapsed"){print a[2]}}}' "${ipi_log_path}" | tail -n 1)"
      ipi_target="$(awk 'index($0,"[ipi_sender]")>0 {for(i=1;i<=NF;i++){split($i,a,"="); if(a[1]=="target"){print a[2]}}}' "${ipi_log_path}" | tail -n 1)"
      ipi_sender="$(awk 'index($0,"[ipi_sender]")>0 {for(i=1;i<=NF;i++){split($i,a,"="); if(a[1]=="sender"){print a[2]}}}' "${ipi_log_path}" | tail -n 1)"
      [[ -z "${ipi_count}" ]] && ipi_count="-"
      [[ -z "${ipi_handled}" ]] && ipi_handled="-"
      [[ -z "${ipi_rate}" ]] && ipi_rate="-"
      [[ -z "${ipi_elapsed}" ]] && ipi_elapsed="-"
      [[ -z "${ipi_target}" ]] && ipi_target="-"
      [[ -z "${ipi_sender}" ]] && ipi_sender="-"
    fi
    local func_overhead_pct="-"
    if [[ "${func_time}" =~ ^[0-9]+$ && "${func_time_base}" =~ ^[0-9]+$ && "${func_time_base}" -gt 0 ]]; then
      func_overhead_pct="$(awk -v i="${func_time}" -v b="${func_time_base}" 'BEGIN{printf("%.2f", (i-b)*100.0/b)}')"
    fi
    printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" \
      "${case}" "${library}" "${version}" "${algorithm}" "${target_branch}" "${instrument_mode}" "${ipi_mode}" "${mode}" "${gs}" "${iterations}" "${run_rc}" "${run_rc_base}" "${prepare_call}" "${prepare_total}" \
      "${check_call}" "${check_total}" "${interrupt_detect}" \
      "${ipi_count}" "${ipi_handled}" "${ipi_rate}" "${ipi_elapsed}" "${ipi_target}" "${ipi_sender}" \
      "${bin_size_base}" "${bin_size_inst}" "${bin_size_delta}" \
      "${time_src}" "${time_unit}" "${total_time}" "${total_time_base}" "${overhead_pct}" \
      "${func_time}" "${func_time_base}" "${func_overhead_pct}" \
      "${log_path}" "${counts_path}" "${log_base_path}" "${log_base_func_path}" "${ipi_log_path}" >> "${out_tsv}"
  done < "${raw_tsv}"
}

append_counts_log_summary() {
  local tsv="$1"
  echo
  echo "## Counts/Log 摘要"
  echo
  echo "_说明：以下统计来自 counts/log 文件本身；若路径不可用则标记为 N/A。_"
  echo
  while IFS=$'\t' read -r case library version algorithm target_branch instrument_mode ipi_mode mode gs iterations rc rc_base prepare prepare_total \
      check check_total interrupt_detect ipi_count ipi_handled ipi_rate ipi_elapsed ipi_target ipi_sender \
      bin_size_base bin_size_inst bin_size_delta time_src time_unit total_time total_time_base overhead_pct \
      func_time func_time_base func_overhead_pct log_path counts_path log_base_path log_base_func_path ipi_log_path; do
    if [[ "${case}" == "case" ]]; then
      continue
    fi
    echo "### ${case} | ${algorithm} | ${mode} | ${gs}"
    echo
    if [[ -n "${counts_path}" && -f "${counts_path}" ]]; then
      local prepare_ids check_pairs top_prepare top_check
      prepare_ids=$(awk '/COUNT prepare/ {for(i=1;i<=NF;i++){split($i,a,"="); if(a[1]=="id"){ids[a[2]]=1}}} END{for(k in ids){c++} print c+0}' "${counts_path}")
      check_pairs=$(awk '/COUNT check/ {id=""; path=""; for(i=1;i<=NF;i++){split($i,a,"="); if(a[1]=="id"){id=a[2]} if(a[1]=="path"){path=a[2]}} if(id!="" && path!=""){pairs[id ":" path]=1}} END{for(k in pairs){c++} print c+0}' "${counts_path}")
      top_prepare="$({ awk '/COUNT prepare/ {id=""; tot=""; for(i=1;i<=NF;i++){split($i,a,"="); if(a[1]=="id"){id=a[2]} if(a[1]=="total"){tot=a[2]}} if(id!="" && tot!=""){printf("%s\t%s\n",id,tot)}}' "${counts_path}" \
        | sort -t$'\t' -k2,2nr | head -n 3 \
        | awk 'BEGIN{ORS=""} {if(NR>1) printf(", "); printf("id=%s:%s",$1,$2)} END{if(NR==0) print "-"}'; } || true)"
      top_check="$({ awk '/COUNT check/ {id=""; path=""; tot=""; for(i=1;i<=NF;i++){split($i,a,"="); if(a[1]=="id"){id=a[2]} if(a[1]=="path"){path=a[2]} if(a[1]=="total"){tot=a[2]}} if(id!="" && path!="" && tot!=""){printf("%s\t%s\t%s\n",id,path,tot)}}' "${counts_path}" \
        | sort -t$'\t' -k3,3nr | head -n 3 \
        | awk 'BEGIN{ORS=""} {if(NR>1) printf(", "); printf("id=%s path=%s:%s",$1,$2,$3)} END{if(NR==0) print "-"}'; } || true)"
      [[ -z "${top_prepare}" ]] && top_prepare="-"
      [[ -z "${top_check}" ]] && top_check="-"
      echo "- counts: prepare_total=${prepare_total}; prepare_ids=${prepare_ids}; check_total=${check_total}; check_pairs=${check_pairs}"
      echo "- top_prepare: ${top_prepare}"
      echo "- top_check: ${top_check}"
    else
      echo "- counts: N/A (文件不存在或不可读: ${counts_path})"
    fi

    if [[ -n "${log_path}" && -f "${log_path}" ]]; then
      local interrupt_count div_count div_ids div_kinds
      interrupt_count="$(awk '/\[intmon\] interrupt_detect/ {for(i=1;i<=NF;i++){split($i,a,"="); if(a[1]=="total"){v=a[2]}}} END{if(v!="") print v}' "${log_path}")"
      if [[ -z "${interrupt_count}" ]]; then
        interrupt_count=$(grep -c "\\[intmon\\] interrupt detect" "${log_path}" || true)
      fi
      div_count=$(grep -c "\\[intmon\\] DIV" "${log_path}" || true)
      div_ids=$(awk '/\[intmon\] DIV/ {for(i=1;i<=NF;i++){split($i,a,"="); if(a[1]=="id"){ids[a[2]]=1}}} END{for(k in ids){list=list (list?",":"") k} print (list==""?"-":list)}' "${log_path}")
      div_kinds=$(awk '/\[intmon\] DIV/ {for(i=1;i<=NF;i++){split($i,a,"="); if(a[1]=="kind"){kinds[a[2]]=1}}} END{for(k in kinds){list=list (list?",":"") k} print (list==""?"-":list)}' "${log_path}")
      echo "- log: interrupt_detect=${interrupt_count}; div=${div_count}; div_ids=${div_ids}; div_kinds=${div_kinds}"
    else
      echo "- log: N/A (文件不存在或不可读: ${log_path})"
    fi
    if [[ -n "${ipi_log_path}" && -f "${ipi_log_path}" ]]; then
      echo "- ipi: count=${ipi_count}; handled=${ipi_handled}; rate=${ipi_rate}; elapsed=${ipi_elapsed}; target=${ipi_target}; sender=${ipi_sender}"
    else
      echo "- ipi: N/A (未启用或文件不可读: ${ipi_log_path})"
    fi
    echo
  done < "${tsv}"
}

append_field_desc() {
  echo
  echo "## 字段说明"
  echo
  cat <<'EOF'
- case: 用例名称
- library/version/algorithm: 库名、版本、密码算法（算法作为最终汇总表的一列元数据）
- target_branch: 目标敏感分支列表（逗号分隔）
- instrument_mode: 插桩模式（branch/once/block/full）
- ipi_mode: IPI 模式（none/kthread/...）
- mode: 运行模式（api/core）
- gs: GS 模式（gs/tls）
- iterations: 重复执行次数
- rc/rc_base: 运行返回码（插桩/基线）
- prepare/check: 静态插桩调用点数量（instrumented.ll 中 __intmon_prepare/__intmon_check 的调用次数）
- prepare_total/check_total: 运行期统计总数（来自 counts_*.txt）
- interrupt_detect: 运行期检测到的中断次数（来自 run_*.log）
- ipi_count: IPI/定时器触发总次数
- ipi_handled: IPI/定时器回调处理次数（apic 模式可能为 0）
- ipi_rate: IPI/定时器实际平均频率（次/秒）
- ipi_elapsed: IPI/定时器触发实际时长（秒）
- ipi_target/ipi_sender: IPI 目标核/发送核
- bin_size_base/bin_size_inst/bin_size_delta: 基线/插桩二进制大小及差值（字节）
- time_src/time_unit: 计时来源与单位（monotonic/ns 或 tsc/cycles）
- total_time/base_total_time: 总耗时（单位见 time_unit）
- func_time/base_func_time: 仅函数计时耗时（单位见 time_unit）
- func_overhead_pct: 插桩相对基线的函数计时开销百分比（%）
- overhead_pct: 插桩相对基线的总耗时开销百分比（%）
- log/counts/log_base/log_base_func/ipi_log: 相关日志文件路径
EOF
}

build_final_tsv "${RAW_TSV}" "${RESULTS_TSV}"

{
  echo "# Known Cases Summary"
  echo
  echo "Generated: $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
  echo
  echo "|case|library|version|algorithm|target_branch|instrument_mode|ipi_mode|mode|gs|iterations|rc|rc_base|prepare|prepare_total|check|check_total|interrupt_detect|ipi_count|ipi_handled|ipi_rate|ipi_elapsed|ipi_target|ipi_sender|bin_size_base|bin_size_inst|bin_size_delta|time_src|time_unit|total_time|base_total_time|overhead_pct|func_time|base_func_time|func_overhead_pct|log|counts|log_base|log_base_func|ipi_log|"
  echo "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"
  while IFS=$'\t' read -r case library version algorithm target_branch instrument_mode ipi_mode mode gs iterations rc rc_base prepare prepare_total \
      check check_total interrupt_detect ipi_count ipi_handled ipi_rate ipi_elapsed ipi_target ipi_sender \
      bin_size_base bin_size_inst bin_size_delta time_src time_unit total_time total_time_base overhead_pct \
      func_time func_time_base func_overhead_pct log_path counts_path log_base_path log_base_func_path ipi_log_path; do
    if [[ "${case}" == "case" ]]; then
      continue
    fi
    printf "|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|\n" \
      "${case}" "${library}" "${version}" "${algorithm}" "${target_branch}" "${instrument_mode}" "${ipi_mode}" "${mode}" "${gs}" "${iterations}" "${rc}" "${rc_base}" "${prepare}" "${prepare_total}" \
      "${check}" "${check_total}" "${interrupt_detect}" \
      "${ipi_count}" "${ipi_handled}" "${ipi_rate}" "${ipi_elapsed}" "${ipi_target}" "${ipi_sender}" \
      "${bin_size_base}" "${bin_size_inst}" "${bin_size_delta}" \
      "${time_src}" "${time_unit}" "${total_time}" "${total_time_base}" "${overhead_pct}" \
      "${func_time}" "${func_time_base}" "${func_overhead_pct}" \
      "${log_path}" "${counts_path}" "${log_base_path}" "${log_base_func_path}" "${ipi_log_path}"
  done < "${RESULTS_TSV}"
  append_field_desc
  append_counts_log_summary "${RESULTS_TSV}"
} > "${SUMMARY_MD}"
echo "汇总报告: ${SUMMARY_MD}"
