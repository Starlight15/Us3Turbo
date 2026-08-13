#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="${SCRIPT_DIR}"
BUILD_DIR="${PROJECT_ROOT}/build"
BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"
JOBS="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"
CLEAN_BUILD=0
BUILD_RTEST="${BUILD_RTEST:-ON}"
# GDS (CUDA cuObj) 通路编译开关，默认 OFF（无 GPU 机器零依赖可编）。
ENABLE_GDS="${US3_TURBO_ACCESS_ENABLE_GDS:-OFF}"
# 首次编译依赖引导：--with-dep 主动安装系统依赖 + third_party 源码重编 +
# （GDS 时）CUDA/cuObj SDK。默认 0，复用已存在的依赖树。
WITH_DEP=0
# CUDA toolkit 根目录（跨机可移植）；留空则 CMake 自动探测。
CUDA_ROOT=""

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
  --enable-gds      Enable GDS (CUDA cuObj) data path
  --disable-gds     Disable GDS data path (default)
  --with-dep        First-time build: install system deps + rebuild third_party
                    from source (+ install CUDA/cuObj SDK when --enable-gds)
  --cuda-root PATH  CUDA toolkit root (default: autodetect)
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
    --enable-gds)
      ENABLE_GDS="ON"
      ;;
    --disable-gds)
      ENABLE_GDS="OFF"
      ;;
    --with-dep)
      WITH_DEP=1
      ;;
    --cuda-root)
      shift
      [[ $# -gt 0 ]] || die "Missing value for --cuda-root"
      CUDA_ROOT="$1"
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

# ---- 首次编译依赖引导 (--with-dep) ----
# 顺序:系统依赖(apt) → third_party 源码重编 → (GDS) CUDA/cuObj SDK。
# 全程不使用不可移植的预编译静态产物;third_party/install 由源码重建。
if [[ ${WITH_DEP} -eq 1 ]]; then
  log "Bootstrap: installing system deps + rebuilding third_party from source"
  bash "${PROJECT_ROOT}/third_party/build_deps.sh" install-system-deps
  bash "${PROJECT_ROOT}/third_party/build_deps.sh" --force
  if [[ ${ENABLE_GDS} == "ON" ]]; then
    log "Bootstrap: installing CUDA / cuObj SDK (GDS)"
    bash "${PROJECT_ROOT}/scripts/install_gds_deps.sh" "${CUDA_VER:-13.1}"
  fi
fi

[[ -d "${FUSION_ACCESS_DEPS_ROOT}" ]] || die "Dependency root not found: ${FUSION_ACCESS_DEPS_ROOT}. Run ./do_make.sh --with-dep first, or set FUSION_ACCESS_DEPS_ROOT"

if [[ ${CLEAN_BUILD} -eq 1 ]]; then
  log "Removing ${BUILD_DIR}"
  rm -rf "${BUILD_DIR}"
fi

mkdir -p "${BUILD_DIR}"

log "Configuring CMake (${BUILD_TYPE})"
CMAKE_ARGS=(
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
  -DFUSION_ACCESS_DEPS_ROOT="${FUSION_ACCESS_DEPS_ROOT}"
  -DUS3_TURBO_ACCESS_BUILD_RTEST="${BUILD_RTEST}"
  -DUS3_TURBO_ACCESS_ENABLE_GDS="${ENABLE_GDS}"
)
[[ -n "${CUDA_ROOT}" ]] && CMAKE_ARGS+=(-DUS3_TURBO_ACCESS_CUDA_ROOT="${CUDA_ROOT}")

cmake -S "${PROJECT_ROOT}" -B "${BUILD_DIR}" "${CMAKE_ARGS[@]}"

log "Building targets with ${JOBS} jobs"
cmake --build "${BUILD_DIR}" -j"${JOBS}"

log "Build finished"
echo "Artifacts: ${BUILD_DIR}"
