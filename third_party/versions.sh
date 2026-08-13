#!/usr/bin/env bash
# FusionAccess third-party dependency versions
# All static dependencies are pinned here.

PROTOBUF_VERSION="25.4"
ABSEIL_VERSION="20230802.1"
BRPC_VERSION="1.11.0"
SPDLOG_VERSION="1.12.0"

# Tarball filenames (shipped in third_party/src/)
PROTOBUF_TARBALL="protobuf-${PROTOBUF_VERSION}.tar.gz"
ABSEIL_TARBALL="abseil-cpp-${ABSEIL_VERSION}.tar.gz"
BRPC_TARBALL="brpc-${BRPC_VERSION}.tar.gz"
SPDLOG_TARBALL="spdlog-${SPDLOG_VERSION}.tar.gz"

# Top-level directory names inside each tarball
PROTOBUF_TARBALL_DIR="protobuf-${PROTOBUF_VERSION}"
ABSEIL_TARBALL_DIR="abseil-cpp-${ABSEIL_VERSION}"
BRPC_TARBALL_DIR="brpc-${BRPC_VERSION}"
SPDLOG_TARBALL_DIR="spdlog-${SPDLOG_VERSION}"
