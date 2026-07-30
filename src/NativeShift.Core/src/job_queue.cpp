#include "nativeshift/core/job_queue.hpp"

#include <algorithm>
#include <exception>
#include <utility>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace nativeshift::core {

struct JobHandle::State {
    std::uint64_t id{};
    std::stop_source cancellation;
    std::promise<ConversionResult> promise;
    std::shared_future<ConversionResult> future{promise.get_future().share()};
    mutable std::mutex job_mutex;
    Job job;
};

JobHandle::JobHandle(std::shared_ptr<State> state) : state_(std::move(state)) {}

void JobHandle::Cancel() const {
    if (state_) {
        state_->cancellation.request_stop();
        std::scoped_lock lock(state_->job_mutex);
        switch (state_->job.state) {
        case JobState::Cancelled:
        case JobState::Completed:
        case JobState::CompletedWithWarnings:
        case JobState::Failed:
            break;
        default:
            state_->job.state = JobState::Cancelling;
            break;
        }
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

Job JobHandle::Snapshot() const {
    if (!state_) {
        return {};
    }
    std::scoped_lock lock(state_->job_mutex);
    return state_->job;
}

JobQueue::JobQueue(ConversionEngine& engine,
                   const std::size_t maximum_concurrent_jobs,
                   const std::size_t maximum_pending_jobs)
    : engine_(engine),
      maximum_pending_jobs_(std::max<std::size_t>(1, maximum_pending_jobs)),
      maximum_resource_weight_(std::clamp<std::size_t>(
          std::max<std::size_t>(1, maximum_concurrent_jobs) * 2, 4, 16)) {
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
    request.job_id = state->id;
    state->job.id = state->id;
    state->job.request = request;
    state->job.state = JobState::Pending;

    {
        std::scoped_lock lock(mutex_);
        if (stopping_ || pending_.size() >= maximum_pending_jobs_) {
            return std::nullopt;
        }
        pending_.push_back(Task{state, std::move(request), std::move(progress),
                                ResourceWeight(state->job.request)});
    }
    condition_.notify_one();
    return JobHandle(std::move(state));
}

std::optional<JobHandle>
JobQueue::Requeue(const JobHandle& completed,
                  std::optional<ConversionRequest> replacement,
                  ProgressCallback progress) {
    if (!completed.state_) {
        return std::nullopt;
    }
    const auto snapshot = completed.Snapshot();
    if (snapshot.state != JobState::Failed &&
        snapshot.state != JobState::Cancelled &&
        snapshot.state != JobState::Completed &&
        snapshot.state != JobState::CompletedWithWarnings) {
        return std::nullopt;
    }
    return TrySubmit(replacement.value_or(snapshot.request),
                     std::move(progress));
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

std::size_t JobQueue::ActiveCount() const {
    std::scoped_lock lock(mutex_);
    return active_.size();
}

std::size_t JobQueue::MaximumConcurrency() const noexcept {
    return workers_.size();
}

std::size_t JobQueue::MaximumResourceWeight() const noexcept {
    return maximum_resource_weight_;
}

std::size_t JobQueue::SafeDefaultConcurrency() noexcept {
    const auto hardware = std::thread::hardware_concurrency();
    auto processor_limit =
        hardware == 0 ? std::size_t{2}
                      : std::clamp<std::size_t>(
                            (static_cast<std::size_t>(hardware) + 1) / 2, 1, 4);
#ifdef _WIN32
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (::GlobalMemoryStatusEx(&memory) != FALSE) {
        constexpr std::uint64_t gibibyte = 1024ULL * 1024ULL * 1024ULL;
        const auto memory_limit = std::clamp<std::size_t>(
            static_cast<std::size_t>(memory.ullAvailPhys / (2 * gibibyte)), 1,
            4);
        processor_limit = std::min(processor_limit, memory_limit);
    }
#endif
    return processor_limit;
}

void JobQueue::WorkerLoop() {
    while (true) {
        Task task;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [this] {
                return stopping_ ||
                       (!paused_ && !pending_.empty() &&
                        (pending_.front()
                             .state->cancellation.stop_requested() ||
                         active_resource_weight_ +
                                 pending_.front().resource_weight <=
                             maximum_resource_weight_)) ||
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
                auto selected =
                    std::ranges::find_if(pending_, [](const auto& candidate) {
                        return candidate.state->cancellation.stop_requested();
                    });
                if (selected == pending_.end()) {
                    selected = pending_.begin();
                }
                task = std::move(*selected);
                pending_.erase(selected);
            }
            active_.push_back(task.state);
            active_resource_weight_ += task.resource_weight;
            {
                std::scoped_lock state_lock(task.state->job_mutex);
                task.state->job.state =
                    task.state->cancellation.stop_requested()
                        ? JobState::Cancelling
                        : JobState::Inspecting;
            }
        }

        ConversionResult result;
        try {
            auto progress = [state = task.state,
                             callback = std::move(task.progress)](
                                const ConversionProgress& update) {
                {
                    std::scoped_lock lock(state->job_mutex);
                    state->job.state = JobState::Converting;
                    state->job.progress = update;
                }
                if (callback) {
                    callback(update);
                }
            };
            {
                std::scoped_lock state_lock(task.state->job_mutex);
                task.state->job.state =
                    task.state->cancellation.stop_requested()
                        ? JobState::Cancelling
                        : JobState::Ready;
            }
            result = engine_.Convert(task.request, std::move(progress),
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

        {
            std::scoped_lock state_lock(task.state->job_mutex);
            result.job_id = task.state->id;
            switch (result.status) {
            case ConversionStatus::Success:
                task.state->job.state = result.warnings.empty()
                                            ? JobState::Completed
                                            : JobState::CompletedWithWarnings;
                break;
            case ConversionStatus::Cancelled:
                task.state->job.state = JobState::Cancelled;
                break;
            case ConversionStatus::Skipped:
                task.state->job.state = JobState::CompletedWithWarnings;
                break;
            case ConversionStatus::Failed:
            default:
                task.state->job.state = JobState::Failed;
                break;
            }
            task.state->job.progress.fraction =
                result.status == ConversionStatus::Success
                    ? 1.0
                    : task.state->job.progress.fraction;
            task.state->job.result = result;
        }
        try {
            task.state->promise.set_value(std::move(result));
        } catch (const std::future_error&) {
        }
        RemoveActive(task.state->id, task.resource_weight);
    }
}

void JobQueue::RemoveActive(const std::uint64_t id,
                            const std::size_t resource_weight) {
    {
        std::scoped_lock lock(mutex_);
        std::erase_if(active_,
                      [id](const auto& state) { return state->id == id; });
        active_resource_weight_ =
            resource_weight > active_resource_weight_
                ? 0
                : active_resource_weight_ - resource_weight;
    }
    condition_.notify_all();
}

std::size_t
JobQueue::ResourceWeight(const ConversionRequest& request) noexcept {
    if (IsVideoFormat(request.input_format) ||
        IsVideoFormat(request.output_format)) {
        return request.video.hardware_acceleration ==
                       HardwareAcceleration::SoftwareOnly
                   ? 4
                   : 2;
    }
    if (IsAudioFormat(request.input_format) ||
        IsAudioFormat(request.output_format)) {
        return 1;
    }
    return 1;
}

} // namespace nativeshift::core
