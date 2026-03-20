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
CROSS_CC="${CROSS_CC:-}"
CROSS_CXX="${CROSS_CXX:-}"
CROSS_AR="${CROSS_AR:-}"
CROSS_RANLIB="${CROSS_RANLIB:-}"
CROSS_STRIP="${CROSS_STRIP:-}"
CROSS_LD="${CROSS_LD:-}"
CROSS_PKG_CONFIG_SYSROOT_DIR="${CROSS_PKG_CONFIG_SYSROOT_DIR:-}"
CROSS_PKG_CONFIG_PATH="${CROSS_PKG_CONFIG_PATH:-}"
CROSS_SYSROOT="${CROSS_SYSROOT:-${SDKTARGETSYSROOT:-${OECORE_TARGET_SYSROOT:-}}}"

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

cross_env=(env)

if [[ -n "${CROSS_CC}" ]]; then
  cross_env+=("CC=${CROSS_CC}")
fi
if [[ -n "${CROSS_CXX}" ]]; then
  cross_env+=("CXX=${CROSS_CXX}")
fi
if [[ -n "${CROSS_AR}" ]]; then
  cross_env+=("AR=${CROSS_AR}")
fi
if [[ -n "${CROSS_RANLIB}" ]]; then
  cross_env+=("RANLIB=${CROSS_RANLIB}")
fi
if [[ -n "${CROSS_STRIP}" ]]; then
  cross_env+=("STRIP=${CROSS_STRIP}")
fi
if [[ -n "${CROSS_LD}" ]]; then
  cross_env+=("LD=${CROSS_LD}")
fi
if [[ -n "${CROSS_PKG_CONFIG_SYSROOT_DIR}" ]]; then
  cross_env+=("PKG_CONFIG_SYSROOT_DIR=${CROSS_PKG_CONFIG_SYSROOT_DIR}")
fi
if [[ -n "${CROSS_PKG_CONFIG_PATH}" ]]; then
  cross_env+=("PKG_CONFIG_PATH=${CROSS_PKG_CONFIG_PATH}")
fi

if [[ -n "${TOOLCHAIN_FILE}" ]]; then
  cross_cmake_args+=( -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" )
elif [[ -n "${CROSS_CC}" && -n "${CROSS_CXX}" ]]; then
  if [[ -n "${CROSS_SYSROOT}" ]]; then
    cross_cmake_args+=( -DCMAKE_SYSROOT="${CROSS_SYSROOT}" )
  fi
elif [[ -f "${TOOLCHAIN_FILE_DEFAULT}" ]]; then
  printf 'Using default toolchain file: %s\n' "${TOOLCHAIN_FILE_DEFAULT}"
  cross_cmake_args+=( -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE_DEFAULT}" )
  if [[ -n "${CROSS_SYSROOT}" ]]; then
    cross_cmake_args+=( -DCMAKE_SYSROOT="${CROSS_SYSROOT}" )
  fi
else
  if [[ -z "${CROSS_CC}" || -z "${CROSS_CXX}" ]]; then
    printf 'Cross stage requires either TOOLCHAIN_FILE or explicit CROSS_CC/CROSS_CXX variables.\n' >&2
    exit 1
  fi
  if [[ -n "${CROSS_SYSROOT}" ]]; then
    cross_cmake_args+=( -DCMAKE_SYSROOT="${CROSS_SYSROOT}" )
  fi
fi

"${cross_env[@]}" cmake "${cross_cmake_args[@]}"

"${cross_env[@]}" cmake --build "${CROSS_BUILD_DIR}" -j"${JOBS}"
"${cross_env[@]}" cmake --install "${CROSS_BUILD_DIR}"

printf 'Dual-stage install ready: %s\n' "${INSTALL_DIR}"
