#pragma once

// buffer_registry.h — 两个内存管理器共享的注册表骨架(模板基类)。
//
// 抽取 GdsMemoryManager / UcxMemoryManager 共有的:
//   注册表(registered_) + 串行化锁(mu_) + 幂等注册/注销流程
// 用模板容纳不同的句柄类型(GDS 存 size_t,UCX 存 ucp_mem_h),真正的 pin/map
// 与释放由派生类通过纯虚钩子 DoRegister/DoUnregister 实现。
//
// RegisterBuffer/UnregisterBuffer 为 protected 骨架:派生类各自暴露公开
// wrapper(GDS 的带 null 校验日志;UCX 不对外暴露,仅 AcquireDescriptor 内部
// 调),从而 null 校验日志文本与重构前逐字一致(见 review/client_refactor_prompt.md
// 阶段2 + 硬约束「行为逐字不变」)。
//
// 本头是「干净」共享头,不含 cuObj / ucp 依赖——两个派生类只各自实例化
// BufferRegistry<size_t> / BufferRegistry<ucp_mem_h>,互不依赖,GDS 回归不会
// 被 UCX 拖累。

#include <cstddef>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace us3_turbo::client {

template <typename Handle>
class BufferRegistry {
 public:
  virtual ~BufferRegistry() = default;

 protected:
  // 幂等注册：已在表内则直接成功；否则调派生类 DoRegister 真正 pin/map 后入表。
  // 不做 null/size 校验(由派生类公开 wrapper 负责,以保留各自日志文本)。
  [[nodiscard]] bool RegisterBuffer(void* ptr, std::size_t size) {
    std::lock_guard<std::mutex> lk(mu_);
    if (registered_.count(ptr)) return true;
    Handle h{};
    if (!DoRegister(ptr, size, h)) return false;
    registered_.emplace(ptr, std::move(h));
    return true;
  }

  // 幂等注销：未注册直接成功；否则调派生类 DoUnregister 释放后出表。
  [[nodiscard]] bool UnregisterBuffer(void* ptr) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = registered_.find(ptr);
    if (it == registered_.end()) return true;   // 幂等
    DoUnregister(ptr, it->second);
    registered_.erase(it);
    return true;
  }

  // 已持锁状态下查询句柄(供 AcquireToken/AcquireDescriptor 等流程用)。
  // 调用方必须已持有 mu_。未注册返回 nullptr。
  [[nodiscard]] const Handle* FindLocked(void* ptr) const {
    auto it = registered_.find(ptr);
    return it == registered_.end() ? nullptr : &it->second;
  }
  [[nodiscard]] Handle* FindLocked(void* ptr) {
    auto it = registered_.find(ptr);
    return it == registered_.end() ? nullptr : &it->second;
  }

  // 析构前批量释放残留句柄(派生类负责逐个 unmap)。供派生类析构函数调用。
  // 调用方负责加锁(与原实现一致:原 GDS 析构不加锁,静态单例程序退出无并发)。
  template <typename Fn>
  void ForEachLocked(Fn&& fn) {
    for (auto& [ptr, h] : registered_) fn(ptr, h);
  }

  // 残留注册数(供派生类析构 warn 日志,与原 registered_.size() 等价)。
  [[nodiscard]] std::size_t RegisteredCount() const noexcept {
    return registered_.size();
  }

  // 清空注册表(供派生类析构里批量 unmap 之后调用)。
  void ClearRegistered() noexcept { registered_.clear(); }

  std::mutex                          mu_;          // 串行化注册表 + 单 worker 访问
  std::unordered_map<void*, Handle>   registered_;

  // —— 细节各自实现(cuObj / ucp 分别在派生类里)——
  // out 由派生类填充(句柄),失败返回 false 并自行记日志(保持原文本)。
  [[nodiscard]] virtual bool DoRegister(void* ptr, std::size_t size,
                                        Handle& out) = 0;
  virtual void DoUnregister(void* ptr, Handle& handle) = 0;
};

}  // namespace us3_turbo::client
