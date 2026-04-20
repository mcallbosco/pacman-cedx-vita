#!/usr/bin/env bash
# Build the loader VPK. Pass any extra cmake -D flags as arguments, e.g.
#   ./build.sh -DENABLE_RUNTIME_LOGS=ON -DVITA_MSAA_MODE=4X
set -euo pipefail

if [[ -z "${VITASDK:-}" ]]; then
    echo "error: VITASDK env var is not set — install VitaSDK from https://vitasdk.org/" >&2
    exit 1
fi

cmake -B build -S . "$@"
cmake --build build -j"$(nproc)"

echo
echo "Built: build/pacmancedx.vpk ($(du -h build/pacmancedx.vpk | cut -f1))"
