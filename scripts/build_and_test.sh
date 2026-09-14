#!/usr/bin/env bash

set -euo pipefail

usage() {
    cat <<'USAGE'
Usage:
  ./scripts/build_and_test.sh [options]

Options:
  --preset NAME       CMake preset: debug, asan, tsan, or release. Default: debug.
  --benchmark         Run minirt_benchmark after building.
  --threads N         Thread count passed to benchmark. Default: benchmark binary default.
  --tasks N           Task count passed to benchmark. Default: benchmark binary default.
  --task-type VALUE   Task type: 0/empty, 1/light, 2/medium, 3/heavy. Default: benchmark binary default.
  --iterations N      Measured benchmark iterations. Default: benchmark binary default.
  --warmup N          Benchmark warmup iterations. Default: benchmark binary default.
  --no-test           Skip ctest after build. Non-release presets still configure test targets.
  --help              Show this help.

Examples:
  ./scripts/build_and_test.sh
  ./scripts/build_and_test.sh --preset asan
  ./scripts/build_and_test.sh --preset tsan
  ./scripts/build_and_test.sh --preset release --benchmark --threads 4 --tasks 100000
USAGE
}

require_value() {
    if [[ $# -lt 2 || "$2" == --* ]]; then
        echo "$1 requires a value" >&2
        usage >&2
        exit 2
    fi

    printf '%s\n' "$2"
}

preset="debug"
run_tests=1
run_benchmark=0
benchmark_threads=""
benchmark_tasks=""
benchmark_task_type=""
benchmark_iterations=""
benchmark_warmup=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --preset)
            preset="$(require_value "$@")"
            shift 2
            ;;
        --preset=*)
            preset="${1#--preset=}"
            shift
            ;;
        --benchmark)
            run_benchmark=1
            shift
            ;;
        --threads)
            benchmark_threads="$(require_value "$@")"
            shift 2
            ;;
        --threads=*)
            benchmark_threads="${1#--threads=}"
            shift
            ;;
        --tasks)
            benchmark_tasks="$(require_value "$@")"
            shift 2
            ;;
        --tasks=*)
            benchmark_tasks="${1#--tasks=}"
            shift
            ;;
        --task-type)
            benchmark_task_type="$(require_value "$@")"
            shift 2
            ;;
        --task-type=*)
            benchmark_task_type="${1#--task-type=}"
            shift
            ;;
        --iterations)
            benchmark_iterations="$(require_value "$@")"
            shift 2
            ;;
        --iterations=*)
            benchmark_iterations="${1#--iterations=}"
            shift
            ;;
        --warmup)
            benchmark_warmup="$(require_value "$@")"
            shift 2
            ;;
        --warmup=*)
            benchmark_warmup="${1#--warmup=}"
            shift
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

case "$preset" in
    debug|asan|tsan|release)
        ;;
    *)
        echo "Unknown preset: $preset" >&2
        usage >&2
        exit 2
        ;;
esac

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
        args+=("--threads" "$benchmark_threads")
    fi
    if [[ -n "$benchmark_tasks" ]]; then
        args+=("--tasks" "$benchmark_tasks")
    fi
    if [[ -n "${benchmark_task_type}" ]]; then
        args+=("--task-type" "$benchmark_task_type")
    fi
    if [[ -n "$benchmark_iterations" ]]; then
        args+=("--iterations" "$benchmark_iterations")
    fi
    if [[ -n "$benchmark_warmup" ]]; then
        args+=("--warmup" "$benchmark_warmup")
    fi

    if [[ "${#args[@]}" -gt 0 ]]; then
        echo "[MiniRuntime] benchmark: $benchmark_path ${args[*]}"
        "$benchmark_path" "${args[@]}"
    else
        echo "[MiniRuntime] benchmark: $benchmark_path"
        "$benchmark_path"
    fi
fi
