#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="${SCRIPT_DIR}"
BUILD_DIR="${PROJECT_ROOT}/build"
BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"
JOBS="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"
CLEAN_BUILD=0
BUILD_EXAMPLES="${BUILD_EXAMPLES:-ON}"

# FUSION_ACCESS_DEPS_ROOT：默认使用代码库内 third_party/install
FUSION_ACCESS_DEPS_ROOT="${FUSION_ACCESS_DEPS_ROOT:-${PROJECT_ROOT}/third_party/install}"

log()  { printf '\033[1;32m>>>\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31mERROR:\033[0m %s\n' "$*" >&2; exit 1; }

usage() {
  cat <<'EOF'
Usage: ./do_make.sh [options]

Options:
  --clean           Remove build/ before configuring
  --debug           Build with Debug
  --release         Build with Release
  --relwithdebinfo  Build with RelWithDebInfo (default)
  --deps-root PATH  Set FusionAccess dependency root
  -j, --jobs N      Set parallel build jobs
  -h, --help        Show this help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --clean)
      CLEAN_BUILD=1
      ;;
    --debug)
      BUILD_TYPE="Debug"
      ;;
    --release)
      BUILD_TYPE="Release"
      ;;
    --relwithdebinfo)
      BUILD_TYPE="RelWithDebInfo"
      ;;
    -j|--jobs)
      shift
      [[ $# -gt 0 ]] || die "Missing value for -j/--jobs"
      JOBS="$1"
      ;;
    --deps-root)
      shift
      [[ $# -gt 0 ]] || die "Missing value for --deps-root"
      FUSION_ACCESS_DEPS_ROOT="$1"
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      die "Unknown option: $1"
      ;;
  esac
  shift
done

[[ -f "${PROJECT_ROOT}/CMakeLists.txt" ]] || die "Run this script from the Us3Turbo repository root"

[[ -d "${FUSION_ACCESS_DEPS_ROOT}" ]] || die "Dependency root not found: ${FUSION_ACCESS_DEPS_ROOT}. Run ./third_party/build_deps.sh first, or set FUSION_ACCESS_DEPS_ROOT"

if [[ ${CLEAN_BUILD} -eq 1 ]]; then
  log "Removing ${BUILD_DIR}"
  rm -rf "${BUILD_DIR}"
fi

mkdir -p "${BUILD_DIR}"

log "Configuring CMake (${BUILD_TYPE})"
cmake -S "${PROJECT_ROOT}" -B "${BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  -DFUSION_ACCESS_DEPS_ROOT="${FUSION_ACCESS_DEPS_ROOT}" \
  -DUS3_TURBO_ACCESS_BUILD_EXAMPLES="${BUILD_EXAMPLES}"

log "Building targets with ${JOBS} jobs"
cmake --build "${BUILD_DIR}" -j"${JOBS}"

log "Build finished"
echo "Artifacts: ${BUILD_DIR}"
