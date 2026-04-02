#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

export AID_RUN_ALL_RESULT_ROOT="${ROOT_DIR}/results"
export AID_RUN_ALL_BASE_BUILD_DIR="${ROOT_DIR}/build-base"

exec "${ROOT_DIR}/scripts/run_all.sh" "$@"
