#include "pch.h"

#include "QueueViewModel.h"

#include "nativeshift/gui_bridge.h"

#include <algorithm>
#include <cwctype>
#include <iomanip>
#include <numeric>
#include <sstream>

namespace NativeShift::presentation {
namespace {

using winrt::Windows::Data::Json::JsonArray;
using winrt::Windows::Data::Json::JsonObject;

std::wstring ReadBridge(std::size_t (*reader)(wchar_t*, std::size_t)) {
    const auto required = reader(nullptr, 0);
    if (required <= 1) {
        return {};
    }
    std::wstring value(required - 1, L'\0');
    (void)reader(value.data(), required);
    return value;
}

std::wstring ReadJob(const std::uint64_t id) {
    const auto required = nativeshift_job_json(id, nullptr, 0);
    if (required <= 1) {
        return {};
    }
    std::wstring value(required - 1, L'\0');
    (void)nativeshift_job_json(id, value.data(), required);
    return value;
}

std::wstring JsonString(const JsonObject& object, const wchar_t* key,
                        std::wstring fallback = {}) {
    if (!object.HasKey(key) ||
        object.GetNamedValue(key).ValueType() !=
            winrt::Windows::Data::Json::JsonValueType::String) {
        return fallback;
    }
    return object.GetNamedString(key).c_str();
}

std::wstring Lowercase(std::wstring value) {
    std::ranges::transform(value, value.begin(), [](const wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return value;
}

std::filesystem::path BuildOutput(const std::filesystem::path& input,
                                  const std::filesystem::path& directory,
                                  const std::wstring_view format) {
    auto stem = input.stem();
    std::wstring extension(format);
    if (extension == L"jpeg") {
        extension = L"jpg";
    }
    auto output = directory / (stem.native() + L"." + extension);
    std::error_code error;
    if (std::filesystem::equivalent(input, output, error) && !error) {
        output = directory / (stem.native() + L" - converted." + extension);
    }
    return output;
}

} // namespace

std::wstring QueueRow::DisplayText() const {
    std::wostringstream stream;
    stream << input_path.filename().native() << L"  |  " << input_format
           << L" -> " << output_format << L"  |  " << state << L"  |  "
           << static_cast<int>(std::clamp(progress, 0.0, 1.0) * 100.0) << L"%";
    if (!codec.empty()) {
        stream << L"  |  " << codec;
    }
    if (!warnings.empty()) {
        stream << L"  |  Warning";
    }
    return stream.str();
}

std::wstring QueueRow::DetailText() const {
    std::wostringstream stream;
    stream << L"Input: " << input_path.native() << L"\nOutput: "
           << output_path.native() << L"\nState: " << state << L"\nProgress: "
           << std::fixed << std::setprecision(1) << progress * 100.0 << L"%";
    if (!stage.empty()) {
        stream << L"\nStage: " << stage;
    }
    if (!codec.empty()) {
        stream << L"\nCodec: " << codec;
    }
    stream << L"\nAcceleration: " << acceleration;
    if (duration_ms > 0) {
        stream << L"\nElapsed: " << duration_ms << L" ms";
    }
    if (!message.empty()) {
        stream << L"\n\n" << message;
    }
    for (const auto& warning : warnings) {
        stream << L"\nWarning: " << warning;
    }
    return stream.str();
}

bool QueueRow::IsTerminal() const noexcept {
    return state == L"completed" || state == L"completed_with_warnings" ||
           state == L"cancelled" || state == L"failed";
}

void QueueViewModel::AddFiles(const std::vector<std::filesystem::path>& paths) {
    for (const auto& path : paths) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error) || error) {
            continue;
        }
        const auto duplicate =
            std::ranges::any_of(rows_, [&path](const auto& existing) {
                std::error_code comparison_error;
                return std::filesystem::equivalent(existing.input_path, path,
                                                   comparison_error) &&
                       !comparison_error;
            });
        if (!duplicate) {
            QueueRow row;
            row.input_path = path;
            rows_.push_back(std::move(row));
        }
    }
}

void QueueViewModel::Start(const std::filesystem::path& output_folder,
                           const std::wstring_view output_format,
                           const std::wstring_view options_json) {
    for (auto& row : rows_) {
        if (row.id != 0 || row.state != L"Ready") {
            continue;
        }
        const auto directory = output_folder.empty()
                                   ? row.input_path.parent_path()
                                   : output_folder;
        row.output_path = BuildOutput(row.input_path, directory, output_format);
        row.output_format = output_format;
        row.id =
            nativeshift_submit(row.input_path.c_str(), row.output_path.c_str(),
                               std::wstring(output_format).c_str(),
                               std::wstring(options_json).c_str());
        if (row.id == 0) {
            row.state = L"failed";
            row.message = LastError();
        } else {
            row.state = L"pending";
        }
    }
}

void QueueViewModel::Refresh() {
    for (auto& row : rows_) {
        if (row.id == 0 || row.IsTerminal()) {
            continue;
        }
        try {
            const auto json = ReadJob(row.id);
            if (json.empty()) {
                continue;
            }
            const auto object = JsonObject::Parse(json);
            row.state = JsonString(object, L"state", row.state);
            row.stage = JsonString(object, L"stage", row.stage);
            row.input_format =
                JsonString(object, L"input_format", row.input_format);
            row.output_format =
                JsonString(object, L"output_format", row.output_format);
            row.progress = object.GetNamedNumber(L"progress", row.progress);
            row.message = JsonString(object, L"message", row.message);
            row.codec = JsonString(object, L"selected_codec", row.codec);
            row.acceleration =
                JsonString(object, L"hardware_acceleration", row.acceleration);
            row.duration_ms = static_cast<std::int64_t>(object.GetNamedNumber(
                L"duration_ms", static_cast<double>(row.duration_ms)));
            row.warnings.clear();
            if (object.HasKey(L"warnings")) {
                const JsonArray values = object.GetNamedArray(L"warnings");
                for (const auto& value : values) {
                    if (value.ValueType() ==
                        winrt::Windows::Data::Json::JsonValueType::String) {
                        row.warnings.emplace_back(value.GetString().c_str());
                    }
                }
            }
        } catch (const winrt::hresult_error& error) {
            row.message = error.message().c_str();
        }
    }
}

void QueueViewModel::Pause() {
    nativeshift_pause_queue();
    paused_ = true;
}

void QueueViewModel::Resume() {
    nativeshift_resume_queue();
    paused_ = false;
}

void QueueViewModel::Cancel(const std::size_t index) {
    if (index < rows_.size() && rows_[index].id != 0) {
        (void)nativeshift_cancel(rows_[index].id);
        rows_[index].state = L"cancelling";
    }
}

void QueueViewModel::CancelAll() { nativeshift_cancel_all(); }

bool QueueViewModel::Retry(const std::size_t index) {
    if (index >= rows_.size() || rows_[index].id == 0 ||
        !rows_[index].IsTerminal()) {
        return false;
    }
    const auto id = nativeshift_retry(rows_[index].id);
    if (id == 0) {
        rows_[index].message = LastError();
        return false;
    }
    rows_[index].id = id;
    rows_[index].state = L"pending";
    rows_[index].stage.clear();
    rows_[index].message.clear();
    rows_[index].codec.clear();
    rows_[index].warnings.clear();
    rows_[index].progress = 0.0;
    rows_[index].duration_ms = 0;
    return true;
}

void QueueViewModel::Remove(const std::size_t index) {
    if (index >= rows_.size()) {
        return;
    }
    if (rows_[index].id == 0 || nativeshift_remove(rows_[index].id)) {
        rows_.erase(rows_.begin() + static_cast<std::ptrdiff_t>(index));
    }
}

void QueueViewModel::ClearCompleted() {
    std::erase_if(rows_, [](const auto& row) {
        if (!row.IsTerminal()) {
            return false;
        }
        return row.id == 0 || nativeshift_remove(row.id);
    });
}

std::vector<std::size_t>
QueueViewModel::FilteredIndices(const std::wstring_view filter,
                                const std::wstring_view state_filter) const {
    const auto normalized_filter = Lowercase(std::wstring(filter));
    const auto normalized_state = Lowercase(std::wstring(state_filter));
    std::vector<std::size_t> result;
    for (std::size_t index = 0; index < rows_.size(); ++index) {
        const auto& row = rows_[index];
        const auto matches_text =
            normalized_filter.empty() ||
            Lowercase(row.input_path.filename().native())
                    .find(normalized_filter) != std::wstring::npos;
        const auto matches_state = normalized_state.empty() ||
                                   normalized_state == L"all" ||
                                   Lowercase(row.state) == normalized_state;
        if (matches_text && matches_state) {
            result.push_back(index);
        }
    }
    return result;
}

const std::vector<QueueRow>& QueueViewModel::Rows() const noexcept {
    return rows_;
}

double QueueViewModel::OverallProgress() const noexcept {
    if (rows_.empty()) {
        return 0.0;
    }
    const auto total =
        std::accumulate(rows_.begin(), rows_.end(), 0.0,
                        [](const double value, const auto& row) {
                            return value + std::clamp(row.progress, 0.0, 1.0);
                        });
    return total / static_cast<double>(rows_.size());
}

std::wstring QueueViewModel::LastError() const {
    return ReadBridge(nativeshift_last_error);
}

bool QueueViewModel::IsPaused() const noexcept { return paused_; }

} // namespace NativeShift::presentation
