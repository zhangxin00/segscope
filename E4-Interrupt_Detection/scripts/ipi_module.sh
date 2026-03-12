#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MOD_NAME="ipi_kmod"
MOD_PATH="${ROOT_DIR}/kernel/ipi_kmod/ipi_kmod.ko"
DEV_PATH="/dev/ipi_ctl"

usage() {
  cat <<USAGE
用法：
  $0 build|install|uninstall|reload|status

说明：
  build     仅编译模块（不需要 sudo）
  install   编译并加载模块，创建/授权 ${DEV_PATH}
  uninstall 卸载模块
  reload    先卸载再安装
  status    查看模块与设备节点状态
USAGE
}

ensure_module_built() {
  if [[ ! -f "${MOD_PATH}" ]]; then
    AID_IPI_BUILD_ONLY=1 "${ROOT_DIR}/scripts/ipi_build_kmod.sh"
  fi
}

cmd_build() {
  AID_IPI_BUILD_ONLY=1 "${ROOT_DIR}/scripts/ipi_build_kmod.sh"
  echo "生成模块: ${MOD_PATH}"
}

cmd_install() {
  "${ROOT_DIR}/scripts/ipi_build_kmod.sh"
  cmd_status
}

cmd_uninstall() {
  if [[ -d "/sys/module/${MOD_NAME}" ]] || ( [[ -r /proc/modules ]] && grep -q "^${MOD_NAME} " /proc/modules ); then
    sudo rmmod "${MOD_NAME}"
  else
    echo "[info] 模块未加载: ${MOD_NAME}"
  fi
}

cmd_reload() {
  cmd_uninstall
  cmd_install
}

cmd_status() {
  if [[ -d "/sys/module/${MOD_NAME}" ]]; then
    echo "[status] 模块已加载: ${MOD_NAME}"
  else
    echo "[status] 模块未加载: ${MOD_NAME}"
  fi
  if [[ -e "${DEV_PATH}" ]]; then
    ls -l "${DEV_PATH}"
  else
    echo "[status] 设备不存在: ${DEV_PATH}"
  fi
}

if [[ $# -lt 1 ]]; then
  usage
  exit 1
fi

case "$1" in
  build)
    cmd_build
    ;;
  install)
    cmd_install
    ;;
  uninstall)
    cmd_uninstall
    ;;
  reload)
    cmd_reload
    ;;
  status)
    cmd_status
    ;;
  -h|--help)
    usage
    ;;
  *)
    echo "未知命令: $1" >&2
    usage >&2
    exit 1
    ;;
 esac
