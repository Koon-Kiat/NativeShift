#include "nativeshift/core/conversion_engine.hpp"

#include "nativeshift/core/format_detector.hpp"
#include "nativeshift/core/output_paths.hpp"
#include "nativeshift/core/validation.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

namespace nativeshift::core {
namespace {

class TemporaryOutputGuard {
  public:
    explicit TemporaryOutputGuard(std::filesystem::path path)
        : path_(std::move(path)) {}

    ~TemporaryOutputGuard() {
        if (armed_) {
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
        }
    }

    void Release() noexcept { armed_ = false; }

  private:
    std::filesystem::path path_;
    bool armed_{true};
};

ConversionResult Failure(const ErrorCategory category, std::string message,
                         const ConversionRequest& request,
                         const std::chrono::steady_clock::time_point started) {
    ConversionResult result;
    result.job_id = request.job_id;
    result.status = category == ErrorCategory::Cancelled
                        ? ConversionStatus::Cancelled
                        : ConversionStatus::Failed;
    result.error = category;
    result.message = std::move(message);
    result.input_format = request.input_format;
    result.output_format = request.output_format;
    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    return result;
}

void ReportProgressSafely(const ProgressCallback& callback,
                          const ConversionProgress& update) noexcept {
    if (!callback) {
        return;
    }
    try {
        callback(update);
    } catch (...) {
        // A presentation-layer progress callback must never fail a job.
    }
}

} // namespace

ConversionEngine::ConversionEngine(Logger* logger, IPlatformServices* platform)
    : logger_(logger), platform_(platform) {}

void ConversionEngine::RegisterProvider(
    std::shared_ptr<IConversionProvider> provider) {
    providers_.Register(std::move(provider));
}

std::shared_ptr<IConversionProvider>
ConversionEngine::SelectProvider(const FileFormat input,
                                 const FileFormat output) const {
    return providers_.Select(input, output);
}

std::vector<ConversionCapabilities> ConversionEngine::Capabilities() const {
    return providers_.Capabilities();
}

ConversionResult ConversionEngine::Convert(ConversionRequest request,
                                           ProgressCallback progress,
                                           const std::stop_token cancellation) {
    const auto started = std::chrono::steady_clock::now();
    const auto finish = [this](ConversionResult&& result) -> ConversionResult {
        if (logger_ != nullptr) {
            logger_->LogConversion(result);
        }
        return std::move(result);
    };

    try {
        if (cancellation.stop_requested()) {
            return finish(Failure(ErrorCategory::Cancelled,
                                  "Conversion was cancelled before it started.",
                                  request, started));
        }

        ReportProgressSafely(progress, {0.0, "Inspecting input"});
        const auto detection = DetectFormat(request.input_path);
        request.input_format = detection.format;

        const auto basic_issues = ValidateRequestBasics(request);
        if (!basic_issues.empty()) {
            return finish(Failure(basic_issues.front().category,
                                  basic_issues.front().message, request,
                                  started));
        }

        const auto provider =
            SelectProvider(request.input_format, request.output_format);
        if (!provider) {
            return finish(
                Failure(ErrorCategory::UnsupportedFormat,
                        "No installed provider supports this conversion.",
                        request, started));
        }

        const auto provider_issues = provider->Validate(request);
        if (!provider_issues.empty()) {
            auto result =
                Failure(provider_issues.front().category,
                        provider_issues.front().message, request, started);
            result.provider = provider->Name();
            return finish(std::move(result));
        }

        const auto resolution =
            ResolveOutputConflict(request.output_path, request.conflict_policy);
        if (!resolution.error.empty()) {
            auto result = Failure(ErrorCategory::Conflict, resolution.error,
                                  request, started);
            result.provider = provider->Name();
            return finish(std::move(result));
        }
        if (resolution.skip) {
            ConversionResult result;
            result.job_id = request.job_id;
            result.status = ConversionStatus::Skipped;
            result.error = ErrorCategory::None;
            result.message = "The output already exists and was skipped.";
            result.provider = provider->Name();
            result.input_format = request.input_format;
            result.output_format = request.output_format;
            result.output_path = resolution.path;
            result.duration =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started);
            return finish(std::move(result));
        }

        request.output_path = resolution.path;
        const auto estimated_bytes = provider->EstimateOutput(request);
        std::error_code space_error;
        StorageSpace space;
        if (platform_ == nullptr) {
            const auto filesystem_space = std::filesystem::space(
                request.output_path.parent_path(), space_error);
            space = {filesystem_space.capacity, filesystem_space.available};
        } else {
            space = platform_->QueryStorageSpace(
                request.output_path.parent_path(), space_error);
        }
        constexpr std::uintmax_t reserve = 16ULL * 1024ULL * 1024ULL;
        if (!space_error &&
            estimated_bytes <=
                std::numeric_limits<std::uintmax_t>::max() - reserve &&
            space.available < estimated_bytes + reserve) {
            auto result = Failure(
                ErrorCategory::ResourceLimit,
                "There is not enough free space for the estimated output.",
                request, started);
            result.provider = provider->Name();
            return finish(std::move(result));
        }

        const auto final_path = request.output_path;
        const auto temporary_path = MakeTemporaryOutputPath(final_path);
        TemporaryOutputGuard guard(temporary_path);
        request.output_path = temporary_path;

        ReportProgressSafely(progress, {0.02, "Starting conversion"});
        const auto safe_progress =
            [progress](const ConversionProgress& update) {
                ReportProgressSafely(progress, update);
            };
        auto outcome = provider->Convert(request, safe_progress, cancellation);

        if (outcome.cancelled || cancellation.stop_requested()) {
            auto result =
                Failure(ErrorCategory::Cancelled,
                        outcome.message.empty() ? "Conversion was cancelled."
                                                : std::move(outcome.message),
                        request, started);
            result.provider = provider->Name();
            result.selected_codec = std::move(outcome.selected_codec);
            result.hardware_acceleration =
                std::move(outcome.hardware_acceleration);
            result.warnings = std::move(outcome.warnings);
            return finish(std::move(result));
        }
        if (!outcome.success) {
            auto result = Failure(
                outcome.error == ErrorCategory::None ? ErrorCategory::Codec
                                                     : outcome.error,
                outcome.message.empty()
                    ? "The provider failed to convert the file."
                    : std::move(outcome.message),
                request, started);
            result.provider = provider->Name();
            result.selected_codec = std::move(outcome.selected_codec);
            result.hardware_acceleration =
                std::move(outcome.hardware_acceleration);
            result.warnings = std::move(outcome.warnings);
            return finish(std::move(result));
        }

        std::string commit_error;
        const bool replace =
            request.conflict_policy == OutputConflictPolicy::Replace;
        if (!CommitTemporaryOutput(temporary_path, final_path, replace,
                                   commit_error)) {
            auto result =
                Failure(ErrorCategory::Io, commit_error, request, started);
            result.provider = provider->Name();
            result.warnings = std::move(outcome.warnings);
            return finish(std::move(result));
        }
        guard.Release();

        ReportProgressSafely(progress, {1.0, "Completed"});
        ConversionResult result;
        result.job_id = request.job_id;
        result.status = ConversionStatus::Success;
        result.error = ErrorCategory::None;
        result.message = "Conversion completed successfully.";
        result.provider = provider->Name();
        result.selected_codec = std::move(outcome.selected_codec);
        result.hardware_acceleration = std::move(outcome.hardware_acceleration);
        result.input_format = request.input_format;
        result.output_format = request.output_format;
        result.output_path = final_path;
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        result.warnings = std::move(outcome.warnings);
        return finish(std::move(result));
    } catch (const std::exception& exception) {
        return finish(Failure(
            ErrorCategory::Internal,
            std::string("A protected conversion boundary caught an error: ") +
                exception.what(),
            request, started));
    } catch (...) {
        return finish(
            Failure(ErrorCategory::Internal,
                    "A protected conversion boundary caught an unknown error.",
                    request, started));
    }
}

} // namespace nativeshift::core
