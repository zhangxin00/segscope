#!/usr/bin/env bash
set -euo pipefail

IMAGE_TAG="${IMAGE_TAG:-intr_detect:llvm18}"
PLATFORM="${PLATFORM:-linux/amd64}"
DOCKERFILE="${DOCKERFILE:-Dockerfile}"
LLVM_TARBALL="${LLVM_TARBALL:-}"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

BUILD_ARGS=()
if [[ -n "${LLVM_TARBALL}" ]]; then
  BUILD_ARGS+=(--build-arg "LLVM_TARBALL=${LLVM_TARBALL}")
fi

docker buildx build --platform "${PLATFORM}" -t "${IMAGE_TAG}" --load \
  -f "${ROOT_DIR}/${DOCKERFILE}" "${BUILD_ARGS[@]}" "${ROOT_DIR}"
docker run --rm -it --platform "${PLATFORM}" \
  -v "${ROOT_DIR}:/work" -w /work \
  "${IMAGE_TAG}" bash
