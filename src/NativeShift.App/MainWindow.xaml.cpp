#include "pch.h"

#include "MainWindow.xaml.h"

#include "nativeshift/gui_bridge.h"

#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include <microsoft.ui.xaml.window.h>
#include <shellapi.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Windows::ApplicationModel::DataTransfer;
using namespace Windows::Storage;
using namespace Windows::Storage::Pickers;

namespace winrt::NativeShift::implementation {
namespace {

std::vector<std::filesystem::path>
EnumerateFolder(const std::filesystem::path& folder, const bool recursive) {
    std::vector<std::filesystem::path> paths;
    std::error_code error;
    constexpr auto options =
        std::filesystem::directory_options::skip_permission_denied;
    if (recursive) {
        std::filesystem::recursive_directory_iterator iterator(folder, options,
                                                               error);
        const std::filesystem::recursive_directory_iterator end;
        while (!error && iterator != end) {
            std::error_code entry_error;
            if (iterator->is_regular_file(entry_error) && !entry_error) {
                paths.push_back(iterator->path());
            }
            iterator.increment(error);
        }
    } else {
        std::filesystem::directory_iterator iterator(folder, options, error);
        const std::filesystem::directory_iterator end;
        while (!error && iterator != end) {
            std::error_code entry_error;
            if (iterator->is_regular_file(entry_error) && !entry_error) {
                paths.push_back(iterator->path());
            }
            iterator.increment(error);
        }
    }
    return paths;
}

std::wstring ReadCapabilities() {
    const auto required = nativeshift_capabilities_json(nullptr, 0);
    if (required <= 1) {
        return L"Capability detection did not return data.";
    }
    std::wstring value(required - 1, L'\0');
    (void)nativeshift_capabilities_json(value.data(), required);
    return value;
}

std::wstring ReadBridgeValue(std::size_t (*reader)(wchar_t*, std::size_t)) {
    const auto required = reader(nullptr, 0);
    if (required <= 1) {
        return {};
    }
    std::wstring value(required - 1, L'\0');
    (void)reader(value.data(), required);
    return value;
}

void SelectComboTag(const ComboBox& combo, const std::wstring_view tag) {
    for (std::uint32_t index = 0; index < combo.Items().Size(); ++index) {
        const auto item = combo.Items().GetAt(index).try_as<ComboBoxItem>();
        if (item && unbox_value_or<hstring>(item.Tag(), L"") == tag) {
            combo.SelectedIndex(static_cast<int>(index));
            return;
        }
    }
}

} // namespace

MainWindow::MainWindow() {
    InitializeComponent();
    StateFilter().SelectionChanged(
        {this, &MainWindow::StateFilter_SelectionChanged});
    PresetSelector().SelectionChanged(
        {this, &MainWindow::PresetSelector_SelectionChanged});
    ::SetWindowPos(WindowHandle(), nullptr, 0, 0, 1180, 800,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (Navigation().MenuItems().Size() > 0) {
        Navigation().SelectedItem(Navigation().MenuItems().GetAt(0));
    }
    refresh_timer_ = DispatcherTimer();
    refresh_timer_.Interval(std::chrono::milliseconds(250));
    refresh_timer_.Tick(
        [weak = get_weak()](IInspectable const&, IInspectable const&) {
            if (const auto self = weak.get()) {
                self->view_model_.Refresh();
                self->RefreshView();
            }
        });
    refresh_timer_.Start();
    LoadSettings();
    RefreshView();
}

HWND MainWindow::WindowHandle() const {
    HWND window{};
    const auto native = this->try_as<::IWindowNative>();
    if (native) {
        check_hresult(native->get_WindowHandle(&window));
    }
    return window;
}

void MainWindow::Navigation_SelectionChanged(
    NavigationView const&,
    NavigationViewSelectionChangedEventArgs const& event) {
    ConvertPage().Visibility(Visibility::Collapsed);
    SettingsPage().Visibility(Visibility::Collapsed);
    CapabilitiesPage().Visibility(Visibility::Collapsed);
    AboutPage().Visibility(Visibility::Collapsed);
    const auto item = event.SelectedItem().try_as<NavigationViewItem>();
    if (!item) {
        return;
    }
    const auto tag = unbox_value_or<hstring>(item.Tag(), L"convert");
    if (tag == L"settings") {
        SettingsPage().Visibility(Visibility::Visible);
    } else if (tag == L"capabilities") {
        CapabilitiesPage().Visibility(Visibility::Visible);
    } else if (tag == L"about") {
        AboutPage().Visibility(Visibility::Visible);
    } else {
        ConvertPage().Visibility(Visibility::Visible);
    }
}

fire_and_forget MainWindow::AddFiles_Click(IInspectable const&,
                                           RoutedEventArgs const&) {
    [[maybe_unused]] const auto lifetime = get_strong();
    FileOpenPicker picker;
    picker.ViewMode(PickerViewMode::List);
    picker.SuggestedStartLocation(PickerLocationId::DocumentsLibrary);
    picker.FileTypeFilter().Append(L"*");
    check_hresult(
        picker.as<IInitializeWithWindow>()->Initialize(WindowHandle()));
    const auto files = co_await picker.PickMultipleFilesAsync();
    std::vector<std::filesystem::path> paths;
    paths.reserve(files.Size());
    for (const auto& file : files) {
        paths.emplace_back(file.Path().c_str());
    }
    AddPaths(paths);
}

fire_and_forget MainWindow::AddFolder_Click(IInspectable const&,
                                            RoutedEventArgs const&) {
    [[maybe_unused]] const auto lifetime = get_strong();
    FolderPicker picker;
    picker.SuggestedStartLocation(PickerLocationId::DocumentsLibrary);
    picker.FileTypeFilter().Append(L"*");
    check_hresult(
        picker.as<IInitializeWithWindow>()->Initialize(WindowHandle()));
    const auto folder = co_await picker.PickSingleFolderAsync();
    if (!folder) {
        co_return;
    }
    const bool recursive = RecursiveFolders().IsChecked().GetBoolean();
    const std::filesystem::path folder_path(folder.Path().c_str());
    apartment_context ui_thread;
    co_await resume_background();
    auto paths = EnumerateFolder(folder_path, recursive);
    co_await ui_thread;
    AddPaths(paths);
}

fire_and_forget MainWindow::BrowseOutput_Click(IInspectable const&,
                                               RoutedEventArgs const&) {
    [[maybe_unused]] const auto lifetime = get_strong();
    FolderPicker picker;
    picker.SuggestedStartLocation(PickerLocationId::DocumentsLibrary);
    picker.FileTypeFilter().Append(L"*");
    check_hresult(
        picker.as<IInitializeWithWindow>()->Initialize(WindowHandle()));
    const auto folder = co_await picker.PickSingleFolderAsync();
    if (folder) {
        OutputFolder().Text(folder.Path());
    }
}

void MainWindow::DropArea_DragOver(IInspectable const&,
                                   DragEventArgs const& event) {
    if (event.DataView().Contains(StandardDataFormats::StorageItems())) {
        event.AcceptedOperation(DataPackageOperation::Copy);
        event.DragUIOverride().Caption(L"Add to NativeShift");
        event.DragUIOverride().IsCaptionVisible(true);
    }
}

fire_and_forget MainWindow::DropArea_Drop(IInspectable const&,
                                          DragEventArgs const& event) {
    [[maybe_unused]] const auto lifetime = get_strong();
    const auto data = event.DataView();
    if (!data.Contains(StandardDataFormats::StorageItems())) {
        co_return;
    }
    const auto items = co_await data.GetStorageItemsAsync();
    std::vector<std::filesystem::path> files;
    std::vector<std::filesystem::path> folders;
    for (const auto& item : items) {
        if (const auto file = item.try_as<StorageFile>()) {
            files.emplace_back(file.Path().c_str());
        } else if (const auto folder = item.try_as<StorageFolder>()) {
            folders.emplace_back(folder.Path().c_str());
        }
    }
    const bool recursive = RecursiveFolders().IsChecked().GetBoolean();
    apartment_context ui_thread;
    co_await resume_background();
    for (const auto& folder : folders) {
        auto discovered = EnumerateFolder(folder, recursive);
        files.insert(files.end(), std::make_move_iterator(discovered.begin()),
                     std::make_move_iterator(discovered.end()));
    }
    co_await ui_thread;
    AddPaths(files);
}

void MainWindow::SearchBox_TextChanged(IInspectable const&,
                                       TextChangedEventArgs const&) {
    RefreshView();
}

void MainWindow::StateFilter_SelectionChanged(
    IInspectable const&, SelectionChangedEventArgs const&) {
    RefreshView();
}

void MainWindow::QueueList_SelectionChanged(IInspectable const&,
                                            SelectionChangedEventArgs const&) {
    const auto selected = SelectedModelIndex();
    if (!selected.has_value()) {
        DetailsText().Text(
            L"Select a queue item to inspect progress, output path, codec, "
            L"warnings, or errors.");
        return;
    }
    DetailsText().Text(view_model_.Rows()[*selected].DetailText());
}

void MainWindow::Convert_Click(IInspectable const&, RoutedEventArgs const&) {
    const auto format = SelectedComboText(OutputFormat());
    if (format.empty()) {
        DetailsText().Text(L"Select an output format before converting.");
        return;
    }
    view_model_.Start(std::filesystem::path(OutputFolder().Text().c_str()),
                      format, BuildOptionsJson());
    RefreshView();
}

void MainWindow::Pause_Click(IInspectable const&, RoutedEventArgs const&) {
    view_model_.Pause();
    DetailsText().Text(
        L"Queue paused. Active conversions will finish; no new jobs start.");
}

void MainWindow::Resume_Click(IInspectable const&, RoutedEventArgs const&) {
    view_model_.Resume();
    DetailsText().Text(L"Queue resumed.");
}

void MainWindow::CancelSelected_Click(IInspectable const&,
                                      RoutedEventArgs const&) {
    if (const auto selected = SelectedModelIndex()) {
        view_model_.Cancel(*selected);
        RefreshView();
    }
}

void MainWindow::CancelAll_Click(IInspectable const&, RoutedEventArgs const&) {
    view_model_.CancelAll();
    RefreshView();
}

void MainWindow::RetrySelected_Click(IInspectable const&,
                                     RoutedEventArgs const&) {
    if (const auto selected = SelectedModelIndex()) {
        if (!view_model_.Retry(*selected)) {
            DetailsText().Text(view_model_.Rows()[*selected].message);
        }
        RefreshView();
    }
}

void MainWindow::RemoveSelected_Click(IInspectable const&,
                                      RoutedEventArgs const&) {
    if (const auto selected = SelectedModelIndex()) {
        view_model_.Remove(*selected);
        RefreshView();
    }
}

void MainWindow::ClearCompleted_Click(IInspectable const&,
                                      RoutedEventArgs const&) {
    view_model_.ClearCompleted();
    RefreshView();
}

void MainWindow::OpenOutput_Click(IInspectable const&, RoutedEventArgs const&) {
    std::filesystem::path path(OutputFolder().Text().c_str());
    if (path.empty()) {
        if (const auto selected = SelectedModelIndex()) {
            path = view_model_.Rows()[*selected].output_path.parent_path();
        }
    }
    std::error_code error;
    if (!path.empty() && std::filesystem::is_directory(path, error) && !error) {
        (void)::ShellExecuteW(WindowHandle(), L"open", path.c_str(), nullptr,
                              nullptr, SW_SHOWNORMAL);
    }
}

void MainWindow::RefreshCapabilities_Click(IInspectable const&,
                                           RoutedEventArgs const&) {
    CapabilitiesText().Text(ReadCapabilities());
}

void MainWindow::PresetSelector_SelectionChanged(
    IInspectable const&, SelectionChangedEventArgs const&) {
    if (!PresetSelector()) {
        return;
    }
    const auto id = SelectedComboText(PresetSelector());
    if (id == L"jpeg-high") {
        SelectOutputFormat(L"jpeg");
        Quality().Value(95);
    } else if (id == L"webp-balanced") {
        SelectOutputFormat(L"webp");
        Quality().Value(82);
    } else if (id == L"mp3-high") {
        SelectOutputFormat(L"mp3");
        SelectComboTag(AudioCodec(), L"mp3");
        AudioBitrate().Value(320);
    } else if (id == L"opus-voice") {
        SelectOutputFormat(L"opus");
        SelectComboTag(AudioCodec(), L"opus");
        AudioBitrate().Value(48);
    } else if (id == L"mp4-h264") {
        SelectOutputFormat(L"mp4");
        SelectComboTag(VideoCodec(), L"h264");
        VideoQuality().Value(23);
    } else if (id == L"webm-vp9") {
        SelectOutputFormat(L"webm");
        SelectComboTag(VideoCodec(), L"vp9");
        VideoQuality().Value(23);
    }
}

void MainWindow::SaveSettings_Click(IInspectable const&,
                                    RoutedEventArgs const&) {
    Windows::Data::Json::JsonObject document;
    document.Insert(L"default_output_folder",
                    Windows::Data::Json::JsonValue::CreateStringValue(
                        OutputFolder().Text()));
    document.Insert(L"maximum_concurrent_conversions",
                    Windows::Data::Json::JsonValue::CreateNumberValue(
                        MaximumJobsSetting().Value()));
    document.Insert(L"theme", Windows::Data::Json::JsonValue::CreateStringValue(
                                  SelectedComboText(ThemeSetting())));
    document.Insert(L"logging_level",
                    Windows::Data::Json::JsonValue::CreateStringValue(
                        SelectedComboText(LoggingSetting())));
    document.Insert(L"notifications_enabled",
                    Windows::Data::Json::JsonValue::CreateBooleanValue(
                        NotificationsSetting().IsOn()));
    if (nativeshift_save_settings(document.Stringify().c_str())) {
        SettingsStatus().Text(
            L"Settings saved. Concurrency and logging changes apply after "
            L"restart.");
    } else {
        SettingsStatus().Text(view_model_.LastError());
    }
}

void MainWindow::ResetSettings_Click(IInspectable const&,
                                     RoutedEventArgs const&) {
    if (nativeshift_reset_settings()) {
        LoadSettings();
        SettingsStatus().Text(L"Settings reset to safe defaults.");
    } else {
        SettingsStatus().Text(view_model_.LastError());
    }
}

void MainWindow::OpenLogs_Click(IInspectable const&, RoutedEventArgs const&) {
    const auto folder = ReadBridgeValue(nativeshift_log_folder);
    if (!folder.empty()) {
        (void)::ShellExecuteW(WindowHandle(), L"open", folder.c_str(), nullptr,
                              nullptr, SW_SHOWNORMAL);
    }
}

void MainWindow::CopyDiagnostics_Click(IInspectable const&,
                                       RoutedEventArgs const&) {
    const auto summary = ReadBridgeValue(nativeshift_diagnostic_summary);
    if (summary.empty()) {
        SettingsStatus().Text(L"A diagnostic summary could not be generated.");
        return;
    }
    DataPackage package;
    package.SetText(summary);
    Clipboard::SetContent(package);
    Clipboard::Flush();
    SettingsStatus().Text(
        L"Privacy-safe diagnostic summary copied to the clipboard.");
}

void MainWindow::RefreshView() {
    const auto selected_model = SelectedModelIndex();
    const auto state_filter = SelectedComboText(StateFilter());
    visible_indices_ =
        view_model_.FilteredIndices(SearchBox().Text().c_str(), state_filter);
    QueueList().Items().Clear();
    int selected_display = -1;
    for (std::size_t display = 0; display < visible_indices_.size();
         ++display) {
        const auto model = visible_indices_[display];
        QueueList().Items().Append(
            box_value(hstring(view_model_.Rows()[model].DisplayText())));
        if (selected_model.has_value() && *selected_model == model) {
            selected_display = static_cast<int>(display);
        }
    }
    QueueList().SelectedIndex(selected_display);
    OverallProgress().Value(view_model_.OverallProgress() * 100.0);
}

void MainWindow::AddPaths(const std::vector<std::filesystem::path>& paths) {
    const auto before = view_model_.Rows().size();
    view_model_.AddFiles(paths);
    const auto added = view_model_.Rows().size() - before;
    DetailsText().Text(L"Added " + std::to_wstring(added) +
                       L" file(s). Unsupported items are reported when "
                       L"conversion inspects their content.");
    RefreshView();
}

std::optional<std::size_t> MainWindow::SelectedModelIndex() {
    const auto selected = QueueList().SelectedIndex();
    if (selected < 0 ||
        static_cast<std::size_t>(selected) >= visible_indices_.size()) {
        return std::nullopt;
    }
    return visible_indices_[static_cast<std::size_t>(selected)];
}

std::wstring MainWindow::SelectedComboText(ComboBox combo) {
    const auto item = combo.SelectedItem().try_as<ComboBoxItem>();
    if (!item) {
        return {};
    }
    if (const auto tag = item.Tag()) {
        return unbox_value_or<hstring>(tag, L"").c_str();
    }
    return unbox_value_or<hstring>(item.Content(), L"").c_str();
}

std::wstring MainWindow::BuildOptionsJson() {
    std::wostringstream json;
    json << LR"({"image_quality":)" << static_cast<int>(Quality().Value())
         << LR"(,"audio_bitrate_kbps":)"
         << static_cast<int>(AudioBitrate().Value()) << LR"(,"video_quality":)"
         << static_cast<int>(VideoQuality().Value()) << LR"(,"audio_codec":")"
         << SelectedComboText(AudioCodec()) << LR"(","video_codec":")"
         << SelectedComboText(VideoCodec()) << LR"(,"preserve_metadata":)"
         << (PreserveMetadata().IsChecked().GetBoolean() ? L"true" : L"false");
    const auto preset = PresetSelector().SelectedItem().try_as<ComboBoxItem>();
    if (preset && preset.Tag()) {
        const auto id = unbox_value_or<hstring>(preset.Tag(), L"");
        if (!id.empty()) {
            json << LR"(,"preset_id":")" << id.c_str() << LR"(")";
        }
    }
    if (!std::isnan(Width().Value())) {
        json << LR"(,"width":)" << static_cast<std::uint32_t>(Width().Value());
    }
    if (!std::isnan(Height().Value())) {
        json << LR"(,"height":)"
             << static_cast<std::uint32_t>(Height().Value());
    }
    json << LR"(,"hardware":")" << SelectedComboText(HardwareMode())
         << LR"(","conflict":")" << SelectedComboText(ConflictPolicy())
         << LR"("})";
    return json.str();
}

void MainWindow::LoadSettings() {
    try {
        const auto json = ReadBridgeValue(nativeshift_settings_json);
        if (json.empty()) {
            return;
        }
        const auto settings = Windows::Data::Json::JsonObject::Parse(json);
        OutputFolder().Text(
            settings.GetNamedString(L"default_output_folder", L""));
        MaximumJobsSetting().Value(
            settings.GetNamedNumber(L"maximum_concurrent_conversions", 2.0));
        NotificationsSetting().IsOn(
            settings.GetNamedBoolean(L"notifications_enabled", true));
        SelectComboTag(ThemeSetting(),
                       settings.GetNamedString(L"theme", L"system").c_str());
        SelectComboTag(
            LoggingSetting(),
            settings.GetNamedString(L"logging_level", L"information").c_str());
        SettingsStatus().Text(settings.GetNamedString(L"warning", L""));
    } catch (const hresult_error& error) {
        SettingsStatus().Text(error.message());
    }
}

void MainWindow::SelectOutputFormat(const std::wstring_view value) {
    for (std::uint32_t index = 0; index < OutputFormat().Items().Size();
         ++index) {
        const auto item =
            OutputFormat().Items().GetAt(index).try_as<ComboBoxItem>();
        if (item && unbox_value_or<hstring>(item.Content(), L"") == value) {
            OutputFormat().SelectedIndex(static_cast<int>(index));
            return;
        }
    }
}

} // namespace winrt::NativeShift::implementation
