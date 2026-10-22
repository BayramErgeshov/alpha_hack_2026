#!/usr/bin/env bash
# Local Linux build helper. Usage: ./build.sh [extra cmake args]
set -euo pipefail
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DALFA_BUILD_EXAMPLES=OFF "$@" >/dev/null
cmake --build build -j"$(nproc)"
