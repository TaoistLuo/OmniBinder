#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
INSTALL_DIR="${BUILD_DIR}/install"
EXAMPLE_SOURCE_DIR="${ROOT_DIR}/examples/artifact_sensor_hmi"
EXAMPLE_BUILD_DIR="${ROOT_DIR}/build/example_sensor_hmi"

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}"
cmake --build "${BUILD_DIR}" -j4
cmake --install "${BUILD_DIR}"

export PATH="${INSTALL_DIR}/bin:${PATH}"

cmake -S "${EXAMPLE_SOURCE_DIR}" -B "${EXAMPLE_BUILD_DIR}" \
  -DOmniBinder_DIR="${INSTALL_DIR}/lib/cmake/OmniBinder"
cmake --build "${EXAMPLE_BUILD_DIR}" -j4

printf 'Downstream example build ready: %s\n' "${EXAMPLE_BUILD_DIR}"
