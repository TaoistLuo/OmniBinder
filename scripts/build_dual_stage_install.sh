#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOST_BUILD_DIR="${ROOT_DIR}/build-host"
CROSS_BUILD_DIR="${ROOT_DIR}/build-cross"
INSTALL_DIR="${ROOT_DIR}/install"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="${JOBS:-$(nproc)}"
TOOLCHAIN_FILE_DEFAULT="${ROOT_DIR}/cmake/toolchains/arm-linux-gnueabihf.cmake"
TOOLCHAIN_FILE="${TOOLCHAIN_FILE:-}"

host_env=(env -u CC -u CXX -u AR -u RANLIB -u STRIP -u LD -u PKG_CONFIG_SYSROOT_DIR -u PKG_CONFIG_PATH -u CROSS_COMPILE -u SDKTARGETSYSROOT -u OECORE_TARGET_SYSROOT -u OECORE_NATIVE_SYSROOT)

"${host_env[@]}" cmake -S "${ROOT_DIR}" -B "${HOST_BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}" \
  -DOMNIBINDER_BUILD_TESTS=OFF \
  -DOMNIBINDER_BUILD_EXAMPLES=OFF

"${host_env[@]}" cmake --build "${HOST_BUILD_DIR}" -j"${JOBS}"
"${host_env[@]}" cmake --install "${HOST_BUILD_DIR}"

if [[ ! -x "${INSTALL_DIR}/bin_host/omni-idlc" ]]; then
  printf 'host omni-idlc not found: %s\n' "${INSTALL_DIR}/bin_host/omni-idlc" >&2
  exit 1
fi

cross_cmake_args=(
  -S "${ROOT_DIR}"
  -B "${CROSS_BUILD_DIR}"
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
  -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}"
  -DOMNIBINDER_BUILD_TESTS=OFF
  -DOMNIBINDER_BUILD_EXAMPLES=OFF
  -DOMNIBINDER_HOST_IDLC="${INSTALL_DIR}/bin_host/omni-idlc"
)

if [[ -z "${CC:-}" || -z "${CXX:-}" ]]; then
  printf 'Cross stage requires CC and CXX in the current environment.\n' >&2
  exit 1
fi

if [[ -n "${TOOLCHAIN_FILE}" ]]; then
  cross_cmake_args+=( -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" )
elif [[ -f "${TOOLCHAIN_FILE_DEFAULT}" ]]; then
  printf 'Using default toolchain file: %s\n' "${TOOLCHAIN_FILE_DEFAULT}"
  cross_cmake_args+=( -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE_DEFAULT}" )
fi

cmake "${cross_cmake_args[@]}"

cmake --build "${CROSS_BUILD_DIR}" -j"${JOBS}"
cmake --install "${CROSS_BUILD_DIR}"

printf 'Dual-stage install ready: %s\n' "${INSTALL_DIR}"
