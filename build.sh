#!/usr/bin/env bash
# Build the loader VPK. Pass any extra cmake -D flags as arguments, e.g.
#   ./build.sh -DENABLE_RUNTIME_LOGS=ON -DVITA_MSAA_MODE=4X
set -euo pipefail

if [[ -z "${VITASDK:-}" ]]; then
    echo "error: VITASDK env var is not set — install VitaSDK from https://vitasdk.org/" >&2
    exit 1
fi

# Start with a quiet Release build even after an earlier diagnostic build.
# Explicit command-line options may still enable the requested diagnostics.
cmake -B build -S . \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_RUNTIME_LOGS=OFF \
    -DENABLE_GL_DEBUG_HOOKS=OFF \
    -DENABLE_FALSOJNI_VERBOSE=OFF \
    -DENABLE_AUDIO_LOGS=OFF \
    -DENABLE_IO_PROFILING=OFF \
    -DDUMP_COMPILED_SHADERS=OFF \
    "$@"
cmake --build build -j"$(nproc)"

echo
echo "Built: build/pacmancedx.vpk ($(du -h build/pacmancedx.vpk | cut -f1))"
