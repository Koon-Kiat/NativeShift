#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace NativeShift::presentation {

enum class ConversionKind { All, Image, Audio, Video, Unknown };

[[nodiscard]] std::wstring_view
ConversionKindName(ConversionKind kind) noexcept;

struct QueueRow {
    std::uint64_t id{};
    std::filesystem::path input_path;
    std::filesystem::path output_path;
    ConversionKind kind{ConversionKind::Unknown};
    std::wstring input_format{L"unknown"};
    std::wstring output_format;
    std::wstring state{L"ready"};
    std::wstring stage;
    std::wstring message;
    std::wstring codec;
    std::wstring acceleration{L"none"};
    std::vector<std::wstring> warnings;
    double progress{};
    std::int64_t duration_ms{};

    [[nodiscard]] std::wstring DisplayText() const;
    [[nodiscard]] std::wstring DetailText() const;
    [[nodiscard]] bool IsTerminal() const noexcept;
};

class QueueViewModel {
  public:
    void AddFiles(const std::vector<std::filesystem::path>& paths);
    [[nodiscard]] std::size_t Start(ConversionKind kind,
                                    const std::filesystem::path& output_folder,
                                    std::wstring_view output_format,
                                    std::wstring_view options_json);
    void Refresh();
    void Pause();
    void Resume();
    void Cancel(std::size_t index);
    void CancelAll();
    bool Retry(std::size_t index);
    void Remove(std::size_t index);
    void ClearCompleted();
    void ClearStaged();

    [[nodiscard]] std::vector<std::size_t>
    FilteredIndices(std::wstring_view filter, std::wstring_view state_filter,
                    ConversionKind kind_filter = ConversionKind::All) const;
    [[nodiscard]] const std::vector<QueueRow>& Rows() const noexcept;
    [[nodiscard]] double OverallProgress() const noexcept;
    [[nodiscard]] std::wstring LastError() const;
    [[nodiscard]] bool IsPaused() const noexcept;

  private:
    std::vector<QueueRow> rows_;
    bool paused_{false};
};

} // namespace NativeShift::presentation
