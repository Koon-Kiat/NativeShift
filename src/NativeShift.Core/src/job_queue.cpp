#include "nativeshift/core/job_queue.hpp"

#include <algorithm>
#include <exception>
#include <utility>

namespace nativeshift::core {

struct JobHandle::State {
    std::uint64_t id{};
    std::stop_source cancellation;
    std::promise<ConversionResult> promise;
    std::shared_future<ConversionResult> future{promise.get_future().share()};
};

JobHandle::JobHandle(std::shared_ptr<State> state) : state_(std::move(state)) {}

void JobHandle::Cancel() const {
    if (state_) {
        state_->cancellation.request_stop();
    }
}

bool JobHandle::IsReady() const {
    return state_ && state_->future.wait_for(std::chrono::milliseconds(0)) ==
                         std::future_status::ready;
}

std::future_status
JobHandle::WaitFor(const std::chrono::milliseconds timeout) const {
    if (!state_) {
        return std::future_status::ready;
    }
    return state_->future.wait_for(timeout);
}

ConversionResult JobHandle::Get() const {
    if (!state_) {
        ConversionResult result;
        result.status = ConversionStatus::Failed;
        result.error = ErrorCategory::Internal;
        result.message = "The job handle is empty.";
        return result;
    }
    return state_->future.get();
}

std::uint64_t JobHandle::Id() const noexcept { return state_ ? state_->id : 0; }

JobQueue::JobQueue(ConversionEngine& engine,
                   const std::size_t maximum_concurrent_jobs,
                   const std::size_t maximum_pending_jobs)
    : engine_(engine),
      maximum_pending_jobs_(std::max<std::size_t>(1, maximum_pending_jobs)) {
    const auto worker_count =
        std::clamp<std::size_t>(maximum_concurrent_jobs, 1, 32);
    workers_.reserve(worker_count);
    for (std::size_t index = 0; index < worker_count; ++index) {
        workers_.emplace_back([this] { WorkerLoop(); });
    }
}

JobQueue::~JobQueue() {
    std::deque<Task> abandoned;
    {
        std::scoped_lock lock(mutex_);
        stopping_ = true;
        abandoned = std::move(pending_);
        for (const auto& state : active_) {
            state->cancellation.request_stop();
        }
    }
    for (auto& task : abandoned) {
        task.state->cancellation.request_stop();
        ConversionResult result;
        result.status = ConversionStatus::Cancelled;
        result.error = ErrorCategory::Cancelled;
        result.message = "The queue was shut down before the job started.";
        try {
            task.state->promise.set_value(std::move(result));
        } catch (const std::future_error&) {
        }
    }
    condition_.notify_all();
    workers_.clear();
}

std::optional<JobHandle> JobQueue::TrySubmit(ConversionRequest request,
                                             ProgressCallback progress) {
    auto state = std::make_shared<JobHandle::State>();
    state->id = next_id_.fetch_add(1);

    {
        std::scoped_lock lock(mutex_);
        if (stopping_ || pending_.size() >= maximum_pending_jobs_) {
            return std::nullopt;
        }
        pending_.push_back(
            Task{state, std::move(request), std::move(progress)});
    }
    condition_.notify_one();
    return JobHandle(std::move(state));
}

void JobQueue::Pause() {
    std::scoped_lock lock(mutex_);
    paused_ = true;
}

void JobQueue::Resume() {
    {
        std::scoped_lock lock(mutex_);
        paused_ = false;
    }
    condition_.notify_all();
}

void JobQueue::CancelAll() {
    std::scoped_lock lock(mutex_);
    for (const auto& task : pending_) {
        task.state->cancellation.request_stop();
    }
    for (const auto& state : active_) {
        state->cancellation.request_stop();
    }
    condition_.notify_all();
}

bool JobQueue::IsPaused() const {
    std::scoped_lock lock(mutex_);
    return paused_;
}

std::size_t JobQueue::PendingCount() const {
    std::scoped_lock lock(mutex_);
    return pending_.size();
}

std::size_t JobQueue::MaximumConcurrency() const noexcept {
    return workers_.size();
}

std::size_t JobQueue::SafeDefaultConcurrency() noexcept {
    const auto hardware = std::thread::hardware_concurrency();
    if (hardware == 0) {
        return 2;
    }
    return std::clamp<std::size_t>((static_cast<std::size_t>(hardware) + 1) / 2,
                                   1, 4);
}

void JobQueue::WorkerLoop() {
    while (true) {
        Task task;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [this] {
                return stopping_ || (!paused_ && !pending_.empty()) ||
                       std::ranges::any_of(pending_, [](const auto& pending) {
                           return pending.state->cancellation.stop_requested();
                       });
            });
            if (stopping_) {
                return;
            }
            if (paused_) {
                const auto cancelled =
                    std::ranges::find_if(pending_, [](const auto& pending) {
                        return pending.state->cancellation.stop_requested();
                    });
                if (cancelled == pending_.end()) {
                    continue;
                }
                task = std::move(*cancelled);
                pending_.erase(cancelled);
            } else {
                task = std::move(pending_.front());
                pending_.pop_front();
            }
            active_.push_back(task.state);
        }

        ConversionResult result;
        try {
            result = engine_.Convert(std::move(task.request),
                                     std::move(task.progress),
                                     task.state->cancellation.get_token());
        } catch (const std::exception& exception) {
            result.status = ConversionStatus::Failed;
            result.error = ErrorCategory::Internal;
            result.message = std::string("The job boundary caught an error: ") +
                             exception.what();
        } catch (...) {
            result.status = ConversionStatus::Failed;
            result.error = ErrorCategory::Internal;
            result.message = "The job boundary caught an unknown error.";
        }

        try {
            task.state->promise.set_value(std::move(result));
        } catch (const std::future_error&) {
        }
        RemoveActive(task.state->id);
    }
}

void JobQueue::RemoveActive(const std::uint64_t id) {
    std::scoped_lock lock(mutex_);
    std::erase_if(active_, [id](const auto& state) { return state->id == id; });
}

} // namespace nativeshift::core
