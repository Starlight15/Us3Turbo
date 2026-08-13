#!/usr/bin/env bash
#
# FusionAccess — offline static dependency builder
#
# All source tarballs are shipped in third_party/src/.  No network required.
#
# Usage:
#   ./third_party/build_deps.sh                      # build all
#   ./third_party/build_deps.sh protobuf             # build one component
#   ./third_party/build_deps.sh brpc
#   ./third_party/build_deps.sh spdlog
#   ./third_party/build_deps.sh verify               # check all artifacts exist
#   ./third_party/build_deps.sh install-system-deps  # apt install system deps
#   ./third_party/build_deps.sh --force              # rebuild everything from scratch
#
# Idempotent: if the install artifact already exists, that step is skipped.
# Pass --force to always rebuild.

set -euo pipefail

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

source "${SCRIPT_DIR}/versions.sh"

TARBALL_DIR="${SCRIPT_DIR}/src"
BUILD_DIR="${SCRIPT_DIR}/build"
INSTALL_DIR="${SCRIPT_DIR}/install"

NPROC="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
FORCE=0

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
log()  { printf '\033[1;32m>>>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mWARN:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31mERROR:\033[0m %s\n' "$*" >&2; exit 1; }

need_tarball() {
  local name="$1"
  [[ -f "${TARBALL_DIR}/${name}" ]] || die "Tarball not found: ${TARBALL_DIR}/${name}"
}

check_system_deps() {
  local missing=()
  for cmd in cmake g++ make; do
    command -v "$cmd" &>/dev/null || missing+=("$cmd")
  done

  local headers=(
    "/usr/include/openssl/ssl.h:libssl-dev"
    "/usr/include/zlib.h:zlib1g-dev"
    "/usr/include/snappy.h:libsnappy-dev"
    "/usr/include/leveldb/db.h:libleveldb-dev"
    "/usr/include/gflags/gflags.h:libgflags-dev"
    "/usr/include/infiniband/verbs.h:libibverbs-dev"
    "/usr/include/rdma/rdma_cma.h:librdmacm-dev"
    "/usr/include/nlohmann/json.hpp:nlohmann-json3-dev"
  )
  for entry in "${headers[@]}"; do
    local hdr="${entry%%:*}" pkg="${entry##*:}"
    [[ -f "$hdr" ]] || missing+=("$pkg")
  done

  local static_libs=(
    "/usr/lib/x86_64-linux-gnu/libssl.a:libssl-dev"
    "/usr/lib/x86_64-linux-gnu/libcrypto.a:libssl-dev"
    "/usr/lib/x86_64-linux-gnu/libz.a:zlib1g-dev"
    "/usr/lib/x86_64-linux-gnu/libsnappy.a:libsnappy-dev"
    "/usr/lib/x86_64-linux-gnu/libleveldb.a:libleveldb-dev"
    "/usr/lib/x86_64-linux-gnu/libgflags.a:libgflags-dev"
  )
  for entry in "${static_libs[@]}"; do
    local lib="${entry%%:*}" pkg="${entry##*:}"
    [[ -f "$lib" ]] || missing+=("${pkg}(.a)")
  done

  if [[ ${#missing[@]} -gt 0 ]]; then
    echo ""
    echo "Missing system dependencies:"
    printf '  - %s\n' "${missing[@]}"
    echo ""
    echo "Install with:"
    echo "  apt install -y build-essential cmake \\"
    echo "    libssl-dev zlib1g-dev libsnappy-dev libleveldb-dev \\"
    echo "    libgflags-dev libibverbs-dev librdmacm-dev nlohmann-json3-dev"
    echo ""
    die "Fix the above before continuing"
  fi
  log "System dependency check passed"
}

# 自动安装系统依赖(apt,需 root)。幂等:apt-get install 已装即跳过。
# 装完再跑一次 check_system_deps 校验,避免"漏装仍继续编译"。
install_system_deps() {
  if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
    die "Need root to install system deps. Run with sudo, or manually run: \
apt install -y build-essential cmake libssl-dev zlib1g-dev libsnappy-dev \
libleveldb-dev libgflags-dev libibverbs-dev librdmacm-dev nlohmann-json3-dev"
  fi
  log "apt-get install system dependencies ..."
  apt-get update -y
  DEBIAN_FRONTEND=noninteractive apt-get install -y \
    build-essential cmake \
    libssl-dev zlib1g-dev libsnappy-dev libleveldb-dev libgflags-dev \
    libibverbs-dev librdmacm-dev nlohmann-json3-dev
  check_system_deps
}

# ---------------------------------------------------------------------------
# Extract
# ---------------------------------------------------------------------------
extract_to() {
  local tarball="$1" dst="$2"
  if [[ -d "$dst" && $FORCE -eq 0 ]]; then
    return
  fi
  rm -rf "$dst"
  mkdir -p "$dst"
  tar xzf "$tarball" -C "$dst" --strip-components=1
}

# ---------------------------------------------------------------------------
# Build: protobuf (+ abseil bundled)
# ---------------------------------------------------------------------------
build_protobuf() {
  local install_prefix="${INSTALL_DIR}/protobuf-${PROTOBUF_VERSION}-install"
  if [[ -f "${install_prefix}/lib/libprotobuf.a" && $FORCE -eq 0 ]]; then
    log "protobuf already installed, skipping (use --force to rebuild)"
    return
  fi

  need_tarball "$PROTOBUF_TARBALL"
  need_tarball "$ABSEIL_TARBALL"

  local src="${BUILD_DIR}/protobuf-${PROTOBUF_VERSION}-src"
  local bld="${BUILD_DIR}/protobuf-${PROTOBUF_VERSION}-bld"

  log "Extracting protobuf ${PROTOBUF_VERSION} ..."
  extract_to "${TARBALL_DIR}/${PROTOBUF_TARBALL}" "$src"

  log "Extracting abseil ${ABSEIL_VERSION} into protobuf/third_party ..."
  rm -rf "${src}/third_party/abseil-cpp"
  mkdir -p "${src}/third_party/abseil-cpp"
  tar xzf "${TARBALL_DIR}/${ABSEIL_TARBALL}" -C "${src}/third_party/abseil-cpp" --strip-components=1

  rm -rf "$bld"
  mkdir -p "$bld"

  log "Building protobuf ${PROTOBUF_VERSION} (static, PIC) ..."
  cd "$bld"
  cmake "$src" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$install_prefix" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -Dprotobuf_BUILD_TESTS=OFF \
    -Dprotobuf_BUILD_SHARED_LIBS=OFF \
    -Dprotobuf_ABSL_PROVIDER=module

  make -j"${NPROC}"
  make install

  # 兼容把静态库装到 lib64 的发行版(CentOS 7 / devtoolset 场景):软链 lib -> lib64,
  # 使下游 CMakeLists(既查 lib 又查 lib64)与本脚本的 lib/ 校验都能命中。
  if [[ -d "${install_prefix}/lib64" && ! -d "${install_prefix}/lib" ]]; then
    ln -s lib64 "${install_prefix}/lib"
  fi

  # Verify
  local absl_count
  absl_count=$(find "$install_prefix/lib" -name 'libabsl_*.a' | wc -l)
  [[ -f "${install_prefix}/lib/libprotobuf.a" ]]   || die "libprotobuf.a not found after install"
  [[ -f "${install_prefix}/lib/libprotoc.a" ]]      || die "libprotoc.a not found after install"
  [[ -f "${install_prefix}/lib/libutf8_range.a" ]]  || die "libutf8_range.a not found after install"
  [[ "$absl_count" -gt 50 ]]                        || die "Only ${absl_count} abseil .a (expected 80+)"

  log "protobuf done: libprotobuf.a + libprotoc.a + utf8 + ${absl_count} abseil libs"
}

# ---------------------------------------------------------------------------
# Build: brpc (static, against our protobuf)
# ---------------------------------------------------------------------------
build_brpc() {
  local protobuf_prefix="${INSTALL_DIR}/protobuf-${PROTOBUF_VERSION}-install"
  local install_prefix="${INSTALL_DIR}/brpc-${BRPC_VERSION}-static"

  if [[ -f "${install_prefix}/lib/libbrpc.a" && $FORCE -eq 0 ]]; then
    log "brpc already installed, skipping (use --force to rebuild)"
    return
  fi

  [[ -f "${protobuf_prefix}/lib/libprotobuf.a" ]] \
    || die "protobuf not built yet — run: $0 protobuf"

  need_tarball "$BRPC_TARBALL"

  local src="${BUILD_DIR}/brpc-${BRPC_VERSION}-src"
  local bld="${BUILD_DIR}/brpc-${BRPC_VERSION}-bld"

  log "Extracting brpc ${BRPC_VERSION} ..."
  extract_to "${TARBALL_DIR}/${BRPC_TARBALL}" "$src"

  # Apply patches
  local patch_dir="${SCRIPT_DIR}/patches"
  for p in "${patch_dir}"/brpc-${BRPC_VERSION}-*.patch; do
    [[ -f "$p" ]] || continue
    log "Applying patch: $(basename "$p")"
    if patch -d "$src" -p1 -N -r - < "$p"; then
      continue
    fi
    if grep -q "req_msg = _sender._messages ? _sender._messages->Request() : nullptr;" \
        "$src/src/brpc/policy/http_rpc_protocol.cpp" \
      && grep -q "CallAfterRpcResp(req_msg, res_msg);" \
        "$src/src/brpc/policy/http_rpc_protocol.cpp"; then
      warn "Patch already present: $(basename "$p")"
      continue
    fi
    die "Failed to apply patch: $(basename "$p")"
  done

  rm -rf "$bld"
  mkdir -p "$bld"

  log "Building brpc ${BRPC_VERSION} (static, PIC, against protobuf ${PROTOBUF_VERSION}) ..."
  cd "$bld"
  cmake "$src" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_CXX_STANDARD=20 \
    -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_PREFIX_PATH="$protobuf_prefix" \
    -DWITH_GLOG=OFF \
    -DWITH_THRIFT=OFF \
    -DBUILD_BRPC_TOOLS=OFF \
    -DBUILD_UNIT_TESTS=OFF

  # Only build the static library target; tool targets may fail to link
  # against pure-static protobuf (utf8_range), which does not affect us.
  make -j"${NPROC}" brpc-static

  rm -rf "$install_prefix"
  mkdir -p "${install_prefix}/lib" "${install_prefix}/include"
  cp "${bld}/output/lib/libbrpc.a" "${install_prefix}/lib/"
  cp -r "${bld}/output/include/"*  "${install_prefix}/include/"

  [[ -f "${install_prefix}/lib/libbrpc.a" ]] || die "libbrpc.a not found after install"
  log "brpc done: libbrpc.a + headers"
}

# ---------------------------------------------------------------------------
# Build: spdlog (header-only install)
# ---------------------------------------------------------------------------
build_spdlog() {
  local install_prefix="${INSTALL_DIR}/spdlog-install"

  if [[ -f "${install_prefix}/include/spdlog/spdlog.h" && $FORCE -eq 0 ]]; then
    log "spdlog already installed, skipping (use --force to rebuild)"
    return
  fi

  need_tarball "$SPDLOG_TARBALL"

  local src="${BUILD_DIR}/spdlog-${SPDLOG_VERSION}-src"
  local bld="${BUILD_DIR}/spdlog-${SPDLOG_VERSION}-bld"

  log "Extracting spdlog ${SPDLOG_VERSION} ..."
  extract_to "${TARBALL_DIR}/${SPDLOG_TARBALL}" "$src"

  rm -rf "$bld"
  mkdir -p "$bld"

  log "Building spdlog ${SPDLOG_VERSION} (header-only install) ..."
  cd "$bld"
  cmake "$src" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_INSTALL_PREFIX="$install_prefix" \
    -DSPDLOG_BUILD_SHARED=OFF \
    -DSPDLOG_BUILD_PIC=ON

  make -j"${NPROC}"
  make install

  [[ -f "${install_prefix}/include/spdlog/spdlog.h" ]] || die "spdlog.h not found after install"
  log "spdlog done: header-only"
}

# ---------------------------------------------------------------------------
# Build all (ordered by dependency)
# ---------------------------------------------------------------------------
build_all() {
  build_protobuf
  build_brpc
  build_spdlog
}

# ---------------------------------------------------------------------------
# Verify
# ---------------------------------------------------------------------------
verify() {
  log "Verifying all static dependencies ..."
  local ok=1

  local protobuf_prefix="${INSTALL_DIR}/protobuf-${PROTOBUF_VERSION}-install"
  local brpc_prefix="${INSTALL_DIR}/brpc-${BRPC_VERSION}-static"
  local spdlog_prefix="${INSTALL_DIR}/spdlog-install"

  local files=(
    "${protobuf_prefix}/lib/libprotobuf.a"
    "${protobuf_prefix}/lib/libprotoc.a"
    "${protobuf_prefix}/lib/libutf8_range.a"
    "${protobuf_prefix}/lib/libutf8_validity.a"
    "${protobuf_prefix}/bin/protoc"
    "${brpc_prefix}/lib/libbrpc.a"
    "${brpc_prefix}/include/brpc/server.h"
    "${spdlog_prefix}/include/spdlog/spdlog.h"
    "/usr/lib/x86_64-linux-gnu/libssl.a"
    "/usr/lib/x86_64-linux-gnu/libcrypto.a"
    "/usr/lib/x86_64-linux-gnu/libz.a"
    "/usr/lib/x86_64-linux-gnu/libsnappy.a"
    "/usr/lib/x86_64-linux-gnu/libleveldb.a"
    "/usr/lib/x86_64-linux-gnu/libgflags.a"
  )

  for f in "${files[@]}"; do
    if [[ -f "$f" ]]; then
      printf "  %-60s \033[32mOK\033[0m\n" "$f"
    else
      printf "  %-60s \033[31mMISSING\033[0m\n" "$f"
      ok=0
    fi
  done

  local absl_count
  absl_count=$(find "${protobuf_prefix}/lib" -name 'libabsl_*.a' 2>/dev/null | wc -l)
  printf "  %-60s %s\n" "abseil static libs" "${absl_count} files"

  # Tarball check
  echo ""
  log "Source tarballs in third_party/src/:"
  for tb in "$PROTOBUF_TARBALL" "$ABSEIL_TARBALL" "$BRPC_TARBALL" "$SPDLOG_TARBALL"; do
    if [[ -f "${TARBALL_DIR}/${tb}" ]]; then
      printf "  %-40s \033[32mOK\033[0m  (%s)\n" "$tb" "$(du -h "${TARBALL_DIR}/${tb}" | cut -f1)"
    else
      printf "  %-40s \033[31mMISSING\033[0m\n" "$tb"
      ok=0
    fi
  done

  echo ""
  if [[ $ok -eq 1 ]]; then
    log "All dependencies verified. Build FusionAccess with:"
    echo ""
    echo "  cd ${PROJECT_ROOT}/build"
    echo "  cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo \\"
    echo "    -DCMAKE_PREFIX_PATH=\"${spdlog_prefix};${protobuf_prefix};/usr/share/cmake/nlohmann_json\""
    echo "  make -j\$(nproc)"
    echo ""
  else
    die "Some dependencies are missing — see above"
  fi
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
for arg in "$@"; do
  [[ "$arg" == "--force" ]] && FORCE=1
done

# Strip --force from positional args
TARGET="all"
for arg in "$@"; do
  [[ "$arg" != "--force" ]] && { TARGET="$arg"; break; }
done

case "$TARGET" in
  protobuf)
    check_system_deps
    build_protobuf
    ;;
  brpc)
    check_system_deps
    build_brpc
    ;;
  spdlog)
    check_system_deps
    build_spdlog
    ;;
  all)
    check_system_deps
    build_all
    verify
    ;;
  verify)
    verify
    ;;
  install-system-deps)
    install_system_deps
    ;;
  clean)
    log "Cleaning build intermediates ..."
    rm -rf "${BUILD_DIR}"
    log "Done. Install artifacts in third_party/install/ are kept."
    ;;
  *)
    echo "Usage: $0 [--force] [all|protobuf|brpc|spdlog|verify|install-system-deps|clean]"
    exit 1
    ;;
esac
