#pragma once

#include <cstddef>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace us3_turbo::client {

/**
 * @brief 内存管理器共享的注册表骨架(模板基类):注册表 + 锁 + 幂等注册/注销,
 *        pin/map 由派生类在 DoRegister/DoUnregister 实现。
 */
template <typename Handle>
class BufferRegistry {
 public:
  virtual ~BufferRegistry() = default;

 protected:
  /** @brief 幂等注册:已在表内直接成功;否则调 DoRegister 后入表。不做 null/size
   * 校验。 */
  [[nodiscard]] bool RegisterBuffer(void* ptr, std::size_t size) {
    std::lock_guard<std::mutex> lk(mu_);
    if (registered_.count(ptr)) return true;
    Handle h{};
    if (!DoRegister(ptr, size, h)) return false;
    registered_.emplace(ptr, std::move(h));
    return true;
  }

  /** @brief 幂等注销:未注册直接成功;否则调 DoUnregister 释放后出表。 */
  [[nodiscard]] bool UnregisterBuffer(void* ptr) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = registered_.find(ptr);
    if (it == registered_.end()) return true;  // 幂等
    DoUnregister(ptr, it->second);
    registered_.erase(it);
    return true;
  }

  /** @brief 已持锁查询句柄(调用方须持 mu_),未注册返回 nullptr。 */
  [[nodiscard]] const Handle* FindLocked(void* ptr) const {
    auto it = registered_.find(ptr);
    return it == registered_.end() ? nullptr : &it->second;
  }
  [[nodiscard]] Handle* FindLocked(void* ptr) {
    auto it = registered_.find(ptr);
    return it == registered_.end() ? nullptr : &it->second;
  }

  /** @brief 析构前批量释放残留句柄。调用方负责加锁。 */
  template <typename Fn>
  void ForEachLocked(Fn&& fn) {
    for (auto& [ptr, h] : registered_) fn(ptr, h);
  }

  /** @brief 当前已注册 buffer 数量。 */
  [[nodiscard]] std::size_t RegisteredCount() const noexcept {
    return registered_.size();
  }

  /** @brief 清空注册表(不释放句柄,调用方须先 ForEachLocked 释放)。 */
  void ClearRegistered() noexcept { registered_.clear(); }

  std::mutex mu_;
  std::unordered_map<void*, Handle> registered_;

  /** @brief 派生类实现:填充 out(句柄),失败返回 false 并自行记日志。 */
  [[nodiscard]] virtual bool DoRegister(void* ptr, std::size_t size,
                                        Handle& out) = 0;
  /** @brief 派生类实现:释放 handle 持有的资源。 */
  virtual void DoUnregister(void* ptr, Handle& handle) = 0;
};

}  // namespace us3_turbo::client
