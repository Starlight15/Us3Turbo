#pragma once

#include <chrono>
#include <random>
#include <thread>

namespace us3_turbo::client {

/**
 * @brief PUT 操作的重试策略。
 *
 * 用于 ExecuteWithRetry：失败时按指数退避 + 随机抖动重试，直到成功或达到
 * @ref max_attempts。默认 3 次（1 次首发 + 2 次重试）。
 */
struct RetryPolicy {
  int                       max_attempts{3};
  std::chrono::milliseconds initial_backoff{std::chrono::milliseconds(100)};
  std::chrono::milliseconds max_backoff{std::chrono::milliseconds(2000)};
};

/**
 * @brief 执行操作，失败时按指数退避策略重试，直到成功或达到最大尝试次数。
 *
 * @tparam Fn 可调用类型，签名为 () -> bool
 * @param policy    重试策略（最大次数、初始/最大退避时间）
 * @param operation 要执行的操作，返回 true 表示成功，false 表示需要重试
 * @return true  操作最终成功（可能是第一次或重试后）
 * @return false 所有尝试均失败
 *
 * @note 使用指数退避 + 随机抖动（jitter）避免惊群效应。线程局部 RNG，
 *       多线程并发重试互不干扰。
 */
template <typename Fn>
bool ExecuteWithRetry(const RetryPolicy& policy, Fn&& operation) {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  std::uniform_real_distribution<double> jitter(0.5, 1.5);

  bool success = operation();
  for (int attempt = 1; attempt < policy.max_attempts; ++attempt) {
    if (success) return true;

    // 退避时间：initial_backoff * 2^(attempt-1)，上限 max_backoff。
    auto backoff = std::chrono::milliseconds{
        static_cast<long long>(
            policy.initial_backoff.count() * (1LL << (attempt - 1)))};
    if (backoff > policy.max_backoff) backoff = policy.max_backoff;

    // 加 ±50% 随机抖动，避免多个客户端同时重试（惊群）。
    backoff = std::chrono::milliseconds{
        static_cast<long long>(static_cast<double>(backoff.count()) * jitter(rng))};

    std::this_thread::sleep_for(backoff);
    success = operation();
  }
  return success;
}

}  // namespace us3_turbo::client
