#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
EXAMPLE_BUILD_DIR="${ROOT_DIR}/build/example_sensor_hmi"
SESSION_NAME="omnibinder-artifact-c"
DETACH=0
SM_PORT=9900

if [[ "${1:-}" == "--detach" ]]; then
    DETACH=1
fi

"${ROOT_DIR}/examples/artifact_sensor_hmi/scripts/build_downstream_example.sh"

if ss -ltn | grep -q ":${SM_PORT} "; then
    printf 'Error: port %s is already in use.\n' "${SM_PORT}" >&2
    printf 'A service_manager (or another process) is already listening there.\n' >&2
    printf 'Please stop the existing process first, or attach to the running stack instead of starting a new one.\n' >&2
    printf 'Useful commands:\n' >&2
    printf '  ss -ltnp | grep :%s\n' "${SM_PORT}" >&2
    printf '  ps -ef | grep "[s]ervice_manager"\n' >&2
    exit 1
fi

tmux kill-session -t "${SESSION_NAME}" 2>/dev/null || true
tmux new-session -d -s "${SESSION_NAME}"
tmux split-window -h -t "${SESSION_NAME}"
tmux split-window -v -t "${SESSION_NAME}:0.0"

tmux send-keys -t "${SESSION_NAME}:0.0" "cd ${BUILD_DIR}/install/bin && ./service_manager" Enter
tmux send-keys -t "${SESSION_NAME}:0.1" "cd ${EXAMPLE_BUILD_DIR}/bin && ./sensor_c" Enter
tmux send-keys -t "${SESSION_NAME}:0.2" "cd ${EXAMPLE_BUILD_DIR}/bin && ./hmi_c" Enter

printf 'Started tmux session %s\n' "${SESSION_NAME}"
printf 'Attach with: tmux attach -t %s\n' "${SESSION_NAME}"
printf 'ServiceManager port: %s\n' "${SM_PORT}"

if [[ ${DETACH} -eq 0 && -t 0 && -t 1 ]]; then
    printf 'Attaching to tmux session now...\n'
    exec tmux attach -t "${SESSION_NAME}"
fi

printf 'Run with --detach to skip auto-attach.\n'
