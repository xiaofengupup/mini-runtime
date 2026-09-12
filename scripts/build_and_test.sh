#!/usr/bin/env bash

set -euo pipefail

usage() {
    cat <<'USAGE'
Usage:
  ./scripts/build_and_test.sh [debug|asan|tsan|release] [options]

Options:
  --benchmark          Run minirt_benchmark after building.
  --threads N         Thread count passed to benchmark. Default: benchmark binary default.
  --tasks N           Task count passed to benchmark. Default: benchmark binary default.
  --no-test           Skip ctest after build. Non-release presets still configure test targets.
  --help              Show this help.

Examples:
  ./scripts/build_and_test.sh
  ./scripts/build_and_test.sh asan
  ./scripts/build_and_test.sh tsan
  ./scripts/build_and_test.sh release --benchmark --threads 4 --tasks 100000
USAGE
}

preset="debug"
run_tests=1
run_benchmark=0
benchmark_threads=""
benchmark_tasks=""

if [[ $# -gt 0 ]]; then
    case "$1" in
        debug|asan|tsan|release)
            preset="$1"
            shift
            ;;
        --help|-h)
            usage
            exit 0
            ;;
    esac
fi

while [[ $# -gt 0 ]]; do
    case "$1" in
        --benchmark)
            run_benchmark=1
            shift
            ;;
        --threads)
            benchmark_threads="${2:?missing value for --threads}"
            shift 2
            ;;
        --tasks)
            benchmark_tasks="${2:?missing value for --tasks}"
            shift 2
            ;;
        --no-test)
            run_tests=0
            shift
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

if [[ "$preset" == "release" ]]; then
    run_tests=0
fi

configure_args=()
if [[ "$preset" != "release" && ! -d third_party/googletest ]]; then
    echo "[MiniRuntime] third_party/googletest not found; CMake FetchContent will download GoogleTest."
    configure_args+=("-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=")
fi

echo "[MiniRuntime] configure: $preset"
if [[ "${#configure_args[@]}" -gt 0 ]]; then
    cmake --preset "$preset" "${configure_args[@]}"
else
    cmake --preset "$preset"
fi

echo "[MiniRuntime] build: $preset"
cmake --build --preset "$preset" -j

if [[ "$run_tests" -eq 1 ]]; then
    echo "[MiniRuntime] test: $preset"
    ctest --preset "$preset"
fi

if [[ "$run_benchmark" -eq 1 ]]; then
    benchmark_path="build/${preset}/minirt_benchmark"
    if [[ ! -x "$benchmark_path" ]]; then
        echo "Benchmark binary not found or not executable: $benchmark_path" >&2
        exit 1
    fi

    args=()
    if [[ -n "$benchmark_threads" ]]; then
        args+=("$benchmark_threads")
    fi
    if [[ -n "$benchmark_tasks" ]]; then
        if [[ -z "$benchmark_threads" ]]; then
            echo "--tasks requires --threads because benchmark positional args are <threads> <tasks>" >&2
            exit 2
        fi
        args+=("$benchmark_tasks")
    fi

    echo "[MiniRuntime] benchmark: $benchmark_path ${args[*]:-}"
    "$benchmark_path" "${args[@]}"
fi
