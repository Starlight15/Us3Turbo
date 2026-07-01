# UCX RDMA demo — 证明 ucp_get_nbx 后端拉取机制可行。
#
# 单线程编译（不 -j，遵 134 保守并行约束；本 demo 编译秒级）。

UCX_CFLAGS := $(shell pkg-config --cflags ucx)
UCX_LIBS   := $(shell pkg-config --libs ucx)

CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra
LDFLAGS  ?=

BINS := source puller

.PHONY: all clean
all: $(BINS)

source: source.cpp
	$(CXX) $(CXXFLAGS) $(UCX_CFLAGS) $< -o $@ $(LDFLAGS) $(UCX_LIBS)

puller: puller.cpp
	$(CXX) $(CXXFLAGS) $(UCX_CFLAGS) $< -o $@ $(LDFLAGS) $(UCX_LIBS)

clean:
	rm -f $(BINS)
