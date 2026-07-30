#pragma once

#include "nativeshift/core/conversion_engine.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace nativeshift::core {

class JobHandle {
  public:
    JobHandle() = default;

    void Cancel() const;
    [[nodiscard]] bool IsReady() const;
    [[nodiscard]] std::future_status
    WaitFor(std::chrono::milliseconds timeout) const;
    [[nodiscard]] ConversionResult Get() const;
    [[nodiscard]] std::uint64_t Id() const noexcept;
    [[nodiscard]] Job Snapshot() const;

  private:
    struct State;
    explicit JobHandle(std::shared_ptr<State> state);

    std::shared_ptr<State> state_;
    friend class JobQueue;
};

class JobQueue {
  public:
    explicit JobQueue(
        ConversionEngine& engine,
        std::size_t maximum_concurrent_jobs = SafeDefaultConcurrency(),
        std::size_t maximum_pending_jobs = 64);
    ~JobQueue();

    JobQueue(const JobQueue&) = delete;
    JobQueue& operator=(const JobQueue&) = delete;

    [[nodiscard]] std::optional<JobHandle>
    TrySubmit(ConversionRequest request, ProgressCallback progress = {});
    [[nodiscard]] std::optional<JobHandle>
    Requeue(const JobHandle& completed,
            std::optional<ConversionRequest> replacement = std::nullopt,
            ProgressCallback progress = {});

    void Pause();
    void Resume();
    void CancelAll();

    [[nodiscard]] bool IsPaused() const;
    [[nodiscard]] std::size_t PendingCount() const;
    [[nodiscard]] std::size_t ActiveCount() const;
    [[nodiscard]] std::size_t MaximumConcurrency() const noexcept;
    [[nodiscard]] std::size_t MaximumResourceWeight() const noexcept;
    [[nodiscard]] static std::size_t SafeDefaultConcurrency() noexcept;

  private:
    struct Task {
        std::shared_ptr<JobHandle::State> state;
        ConversionRequest request;
        ProgressCallback progress;
        std::size_t resource_weight{1};
    };

    void WorkerLoop();
    void RemoveActive(std::uint64_t id, std::size_t resource_weight);
    [[nodiscard]] static std::size_t
    ResourceWeight(const ConversionRequest& request) noexcept;

    ConversionEngine& engine_;
    const std::size_t maximum_pending_jobs_;
    const std::size_t maximum_resource_weight_;
    std::vector<std::jthread> workers_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<Task> pending_;
    std::vector<std::shared_ptr<JobHandle::State>> active_;
    std::size_t active_resource_weight_{0};
    bool paused_{false};
    bool stopping_{false};
    std::atomic<std::uint64_t> next_id_{1};
};

} // namespace nativeshift::core
