#!/usr/bin/env bash
#
# install_gds_deps.sh — 一键安装 GDS (CUDA + cuObj SDK) 编译依赖
#
# 供 do_make.sh --with-dep --enable-gds 调用,也可单独执行。
# 依赖 NVIDIA 官方 CUDA apt 仓库(需 root + 网络,下载量 GB 级)。
#
# Usage:
#   ./scripts/install_gds_deps.sh [CUDA_VER]   # CUDA_VER 默认 13.1
#
# 安装内容(对应 GDS 编译阻塞清单 A 项):
#   - cuda-toolkit-<ver>       → cudart + cuda.h / cuda_runtime.h + nvcc
#   - cuda-driver-dev-<ver>    → stubs/libcuda.so (driver stub)
#   - nvidia-gds               → GPU Direct Storage 元包
#   - libcufile-dev-<ver>      → cufile.h + libcufile
#   - libcuobjclient-dev-<ver> → cuobjclient.h + libcuobjclient.so
#   - libcuobjserver(-dev)     → libcuobjserver.so + cuobjserver.h (backend 侧)
#   - gds-tools-<ver>          → gdscheck 等诊断工具
#
# 幂等:已装齐则直接退出,不重复下载。

set -euo pipefail

CUDA_VER="${1:-13.1}"
# "13.1" → "13-1"(apt 包名后缀用连字符)
PKG_VER="${CUDA_VER//./-}"

log() { printf '\033[1;32m>>>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mWARN:\033[0m %s\n' "$*" >&2; }
die() { printf '\033[1;31mERROR:\033[0m %s\n' "$*" >&2; exit 1; }

CUDA_ROOT="/usr/local/cuda"

# NVIDIA CUDA 仓库域名:developer.download.nvidia.com 在国内常被墙/极慢,
# 默认走官方国内镜像 .cn;海外可用 NVIDIA_MIRROR=https://developer.download.nvidia.com 覆盖。
NVIDIA_MIRROR="${NVIDIA_MIRROR:-https://developer.download.nvidia.cn}"
NVIDIA_MIRROR_HOST="${NVIDIA_MIRROR#https://}"

# 校验关键编译产物是否齐备(头 + 库 + driver stub)。
gds_present() {
  [[ -f "${CUDA_ROOT}/include/cuda_runtime.h" ]] &&
  [[ -f "${CUDA_ROOT}/include/cuda.h" ]] &&
  [[ -f "${CUDA_ROOT}/include/cufile.h" ]] &&
  [[ -f "${CUDA_ROOT}/include/cuobjclient.h" ]] &&
  [[ -f "${CUDA_ROOT}/targets/x86_64-linux/lib/libcuobjclient.so" ]] &&
  [[ -f "${CUDA_ROOT}/targets/x86_64-linux/lib/stubs/libcuda.so" ]]
}

if gds_present; then
  log "CUDA + cuObj SDK already present (${CUDA_ROOT}), skipping install"
  exit 0
fi

[[ ${EUID:-$(id -u)} -eq 0 ]] || die "Need root to install CUDA/cuObj SDK. Re-run with sudo."

# 仅支持 Ubuntu(apt + NVIDIA 官方仓库);其它发行版给精确指引后退出。
. /etc/os-release
case "${ID:-}-${VERSION_ID:-}" in
  ubuntu-22.04) UBUNTU_VER=2204 ;;
  ubuntu-24.04) UBUNTU_VER=2404 ;;
  *)
    die "Unsupported distro ${ID} ${VERSION_ID}. Install manually: \
cuda-toolkit-${PKG_VER} cuda-driver-dev-${PKG_VER} nvidia-gds \
libcufile-dev-${PKG_VER} libcuobjclient-dev-${PKG_VER} libcuobjserver libcuobjserver-dev"
    ;;
esac

# 加 NVIDIA CUDA 仓库(官方 keyring .deb 同时装 keyring + sources.list)。
REPO_FILE="/etc/apt/sources.list.d/cuda-ubuntu${UBUNTU_VER}-x86_64.list"
if [[ ! -f "${REPO_FILE}" ]]; then
  log "Adding NVIDIA CUDA apt repo (ubuntu${UBUNTU_VER}, mirror ${NVIDIA_MIRROR}) ..."
  apt-get update -y
  apt-get install -y wget
  keyring_deb="/tmp/cuda-keyring_1.1-1_all.deb"
  wget -qO "${keyring_deb}" \
    "${NVIDIA_MIRROR}/compute/cuda/repos/ubuntu${UBUNTU_VER}/x86_64/cuda-keyring_1.1-1_all.deb"
  dpkg -i "${keyring_deb}"
  rm -f "${keyring_deb}"
fi

# 官方 keyring .deb 写入的是 developer.download.nvidia.com;按所选镜像统一改写
# (幂等:已是目标域名则跳过,把 .com 修成 .cn 后再 apt update 才能连通)。
if grep -qs "developer\.download\.nvidia\.com" "${REPO_FILE}" 2>/dev/null; then
  sed -i "s#developer\.download\.nvidia\.com#${NVIDIA_MIRROR_HOST}#g" "${REPO_FILE}"
  log "Rewrote NVIDIA apt repo host to ${NVIDIA_MIRROR_HOST}"
fi

log "Installing CUDA ${CUDA_VER} toolkit + GDS/cuObj SDK (this is a large download) ..."
apt-get update -y
DEBIAN_FRONTEND=noninteractive apt-get install -y \
  "cuda-toolkit-${PKG_VER}" \
  "cuda-driver-dev-${PKG_VER}" \
  "nvidia-gds" \
  "libcufile-dev-${PKG_VER}" \
  "libcuobjclient-dev-${PKG_VER}" \
  "libcuobjserver" \
  "libcuobjserver-dev" \
  "gds-tools-${PKG_VER}"

# 安装后校验;部分包可能把 include 装到版本化目录,确认 alternatives 软链已建。
if gds_present; then
  log "GDS deps installed and verified under ${CUDA_ROOT}"
  echo "  cuda.h/cuda_runtime.h/cufile.h/cuobjclient.h : ${CUDA_ROOT}/include"
  echo "  libcuobjclient.so / libcudart*.a               : ${CUDA_ROOT}/targets/x86_64-linux/lib"
  echo "  libcuda.so (driver stub)                       : ${CUDA_ROOT}/targets/x86_64-linux/lib/stubs"
else
  warn "Install finished but some GDS artifacts not found under ${CUDA_ROOT}."
  warn "Check /usr/local/cuda-* and re-run this script, or pass -DUS3_TURBO_ACCESS_CUDA_ROOT to CMake."
fi
