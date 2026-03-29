#!/usr/bin/env bash
# run_benchmarks.sh — Build and run MDP framing benchmarks
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"

echo "==> Configuring..."
cmake -S "${PROJECT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -1

echo "==> Building bench_mdp..."
cmake --build "${BUILD_DIR}" --target bench_mdp 2>&1 | tail -2

echo ""
"${BUILD_DIR}/bench_mdp"
