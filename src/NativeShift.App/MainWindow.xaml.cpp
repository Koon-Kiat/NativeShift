#include "pch.h"

#include "MainWindow.xaml.h"

#include "nativeshift/gui_bridge.h"

#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include <dwmapi.h>
#include <microsoft.ui.xaml.window.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
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
    QueueTypeFilter().SelectionChanged(
        {this, &MainWindow::QueueTypeFilter_SelectionChanged});
    ImagePresetSelector().SelectionChanged(
        {this, &MainWindow::PresetSelector_SelectionChanged});
    AudioPresetSelector().SelectionChanged(
        {this, &MainWindow::PresetSelector_SelectionChanged});
    VideoPresetSelector().SelectionChanged(
        {this, &MainWindow::PresetSelector_SelectionChanged});
    ExtendsContentIntoTitleBar(true);
    SetTitleBar(AppTitleBar());

    constexpr COLORREF border = RGB(66, 66, 66);
    (void)::DwmSetWindowAttribute(WindowHandle(), DWMWA_BORDER_COLOR, &border,
                                  sizeof(border));
    ::SetWindowPos(WindowHandle(), nullptr, 0, 0, 1280, 820,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    RootLayout().SizeChanged(
        [weak = get_weak()](IInspectable const&,
                            SizeChangedEventArgs const& event) {
            if (const auto self = weak.get();
                self && event.NewSize().Width < 820.0 && self->sidebar_open_) {
                self->SetSidebarOpen(false);
            }
        });
    refresh_timer_ = DispatcherTimer();
    refresh_timer_.Interval(std::chrono::milliseconds(500));
    refresh_timer_.Tick(
        [weak = get_weak()](IInspectable const&, IInspectable const&) {
            if (const auto self = weak.get()) {
                self->view_model_.Refresh();
                self->RefreshView();
            }
        });
    refresh_timer_.Start();
    LoadSettings();
    ApplyConversionKind(::NativeShift::presentation::ConversionKind::Unknown);
    NavigateTo(L"convert", true);
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

void MainWindow::NavigateTo(const std::wstring_view tag,
                            const bool record_history) {
    ConvertPage().Visibility(Visibility::Collapsed);
    QueuePage().Visibility(Visibility::Collapsed);
    SettingsPage().Visibility(Visibility::Collapsed);
    CapabilitiesPage().Visibility(Visibility::Collapsed);
    AboutPage().Visibility(Visibility::Collapsed);

    std::wstring title = L"Convert";
    if (tag == L"queue") {
        title = L"Queue";
        QueuePage().Visibility(Visibility::Visible);
    } else if (tag == L"settings") {
        title = L"Settings";
        SettingsPage().Visibility(Visibility::Visible);
    } else if (tag == L"capabilities") {
        title = L"Capabilities";
        CapabilitiesPage().Visibility(Visibility::Visible);
    } else if (tag == L"about") {
        title = L"About";
        AboutPage().Visibility(Visibility::Visible);
    } else {
        ConvertPage().Visibility(Visibility::Visible);
    }
    UpdateSidebarSelection(tag);

    if (record_history) {
        const std::wstring value(tag);
        if (navigation_history_.empty()) {
            navigation_history_.push_back(value);
            navigation_position_ = 0;
        } else if (navigation_history_[navigation_position_] != value) {
            navigation_history_.erase(
                navigation_history_.begin() +
                    static_cast<std::ptrdiff_t>(navigation_position_ + 1),
                navigation_history_.end());
            navigation_history_.push_back(value);
            navigation_position_ = navigation_history_.size() - 1;
        }
        UpdateNavigationButtons();
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
        DefaultOutputFolderSetting().Text(folder.Path());
        OutputLocationText().Text(folder.Path());
        SettingsStatus().Text(
            L"Select Save settings to use this output folder.");
    }
}

void MainWindow::ToggleSidebar_Click(IInspectable const&,
                                     RoutedEventArgs const&) {
    SetSidebarOpen(!sidebar_open_);
}

void MainWindow::SidebarNavigation_Click(IInspectable const& sender,
                                         RoutedEventArgs const&) {
    const auto element = sender.try_as<FrameworkElement>();
    if (element) {
        NavigateTo(unbox_value_or<hstring>(element.Tag(), L"convert").c_str(),
                   true);
    }
}

void MainWindow::MenuNavigate_Click(IInspectable const& sender,
                                    RoutedEventArgs const&) {
    const auto element = sender.try_as<FrameworkElement>();
    if (element) {
        NavigateTo(unbox_value_or<hstring>(element.Tag(), L"convert").c_str(),
                   true);
    }
}

void MainWindow::Back_Click(IInspectable const&, RoutedEventArgs const&) {
    if (navigation_history_.empty() || navigation_position_ == 0) {
        return;
    }
    --navigation_position_;
    SelectNavigationTag(navigation_history_[navigation_position_]);
    UpdateNavigationButtons();
}

void MainWindow::Forward_Click(IInspectable const&, RoutedEventArgs const&) {
    if (navigation_history_.empty() ||
        navigation_position_ + 1 >= navigation_history_.size()) {
        return;
    }
    ++navigation_position_;
    SelectNavigationTag(navigation_history_[navigation_position_]);
    UpdateNavigationButtons();
}

void MainWindow::OpenQueue_Click(IInspectable const&, RoutedEventArgs const&) {
    ShowQueuePage();
}

void MainWindow::ToggleOptions_Click(IInspectable const&,
                                     RoutedEventArgs const&) {
    ConvertPage().IsPaneOpen(!ConvertPage().IsPaneOpen());
}

void MainWindow::NewConversion_Click(IInspectable const&,
                                     RoutedEventArgs const&) {
    ResetCurrentSession(L"");
    NavigateTo(L"convert", true);
}

void MainWindow::ClearCurrentSession_Click(IInspectable const&,
                                           RoutedEventArgs const&) {
    ResetCurrentSession(
        L"Current files removed. Conversion history remains in Queue.");
}

void MainWindow::SidebarJob_ItemClick(IInspectable const&,
                                      ItemClickEventArgs const& event) {
    std::uint32_t sidebar_index{};
    if (!SidebarJobsList().Items().IndexOf(event.ClickedItem(),
                                           sidebar_index) ||
        sidebar_index >= sidebar_job_indices_.size()) {
        return;
    }
    const auto model_index = sidebar_job_indices_[sidebar_index];
    SearchBox().Text(L"");
    QueueTypeFilter().SelectedIndex(0);
    StateFilter().SelectedIndex(0);
    ShowQueuePage();
    RefreshView();
    const auto found = std::ranges::find(visible_indices_, model_index);
    if (found != visible_indices_.end()) {
        QueueList().SelectedIndex(
            static_cast<int>(std::distance(visible_indices_.begin(), found)));
    }
}

void MainWindow::SidebarJobAction_Click(IInspectable const& sender,
                                        RoutedEventArgs const& event) {
    (void)event;
    const auto button = sender.try_as<Button>();
    if (!button) {
        return;
    }
    const std::wstring tag = unbox_value_or<hstring>(button.Tag(), L"").c_str();
    const auto separator = tag.find(L':');
    if (separator == std::wstring::npos) {
        return;
    }

    std::uint64_t id{};
    try {
        id = std::stoull(tag.substr(separator + 1));
    } catch (...) {
        return;
    }
    const auto found = std::ranges::find_if(
        view_model_.Rows(), [id](const auto& row) { return row.id == id; });
    if (found == view_model_.Rows().end()) {
        return;
    }
    const auto index = static_cast<std::size_t>(
        std::distance(view_model_.Rows().begin(), found));
    const auto action = std::wstring_view(tag).substr(0, separator);

    if (action == L"pin") {
        if (!pinned_jobs_.erase(id)) {
            pinned_jobs_.insert(id);
        }
    } else if (action == L"archive") {
        pinned_jobs_.erase(id);
        archived_jobs_.insert(id);
    } else if (action == L"delete" && found->IsTerminal()) {
        pinned_jobs_.erase(id);
        archived_jobs_.erase(id);
        view_model_.Remove(index);
    }
    sidebar_job_texts_.clear();
    RefreshView();
}

Grid MainWindow::BuildSidebarJobItem(
    const ::NativeShift::presentation::QueueRow& row, const bool pinned) {
    Grid item;
    item.Background(RootLayout()
                        .Resources()
                        .Lookup(box_value(L"TransparentBrush"))
                        .as<Media::Brush>());
    item.MinHeight(38);

    StackPanel labels;
    labels.Margin(Thickness{0, 0, 82, 0});
    TextBlock name;
    name.Text(row.input_path.filename().native());
    name.FontSize(12);
    name.TextTrimming(TextTrimming::CharacterEllipsis);
    TextBlock state;
    auto state_text = row.state;
    std::ranges::replace(state_text, L'_', L' ');
    state.Text((pinned ? L"Pinned · " : L"") + state_text);
    state.FontSize(11);
    state.Opacity(0.66);
    labels.Children().Append(name);
    labels.Children().Append(state);
    item.Children().Append(labels);

    StackPanel actions;
    actions.Orientation(Orientation::Horizontal);
    actions.Spacing(1);
    actions.HorizontalAlignment(HorizontalAlignment::Right);
    actions.VerticalAlignment(VerticalAlignment::Center);
    actions.Opacity(0);
    actions.IsHitTestVisible(false);
    const auto action_style =
        RootLayout()
            .Resources()
            .Lookup(box_value(L"SidebarJobActionButtonStyle"))
            .as<Style>();

    const auto add_action = [&](const std::wstring_view action,
                                const wchar_t* glyph,
                                const std::wstring_view label,
                                const bool enabled = true) {
        Button button;
        button.Style(action_style);
        button.Tag(box_value(
            hstring(std::wstring(action) + L":" + std::to_wstring(row.id))));
        button.IsEnabled(enabled);
        FontIcon icon;
        icon.Glyph(glyph);
        icon.FontSize(12);
        button.Content(icon);
        Automation::AutomationProperties::SetName(button, hstring(label));
        ToolTipService::SetToolTip(button, box_value(hstring(label)));
        button.Click({this, &MainWindow::SidebarJobAction_Click});
        actions.Children().Append(button);
    };
    add_action(L"pin", pinned ? L"\uE77A" : L"\uE718",
               pinned ? L"Unpin job" : L"Pin job");
    add_action(L"archive", L"\uE7B8", L"Archive from sidebar");
    add_action(L"delete", L"\uE74D", L"Delete job", row.IsTerminal());
    item.Children().Append(actions);

    item.PointerEntered(
        [actions](IInspectable const&,
                  Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&) {
            actions.Opacity(1);
            actions.IsHitTestVisible(true);
        });
    item.PointerExited(
        [actions](IInspectable const&,
                  Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&) {
            actions.Opacity(0);
            actions.IsHitTestVisible(false);
        });
    return item;
}

void MainWindow::DropArea_DragOver(IInspectable const&,
                                   DragEventArgs const& event) {
    if (event.DataView().Contains(StandardDataFormats::StorageItems())) {
        event.AcceptedOperation(DataPackageOperation::Copy);
        event.DragUIOverride().Caption(L"Add to NativeShift");
        event.DragUIOverride().IsCaptionVisible(true);
        event.Handled(true);
    }
}

fire_and_forget MainWindow::DropArea_Drop(IInspectable const&,
                                          DragEventArgs const& event) {
    [[maybe_unused]] const auto lifetime = get_strong();
    const auto data = event.DataView();
    if (!data.Contains(StandardDataFormats::StorageItems())) {
        co_return;
    }
    event.Handled(true);
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

void MainWindow::QueueTypeFilter_SelectionChanged(
    IInspectable const&, SelectionChangedEventArgs const&) {
    RefreshView();
}

void MainWindow::DetectedType_Click(IInspectable const& sender,
                                    RoutedEventArgs const&) {
    const auto element = sender.try_as<FrameworkElement>();
    if (!element) {
        return;
    }
    const auto value = unbox_value_or<hstring>(element.Tag(), L"");
    if (value == L"audio") {
        ApplyConversionKind(::NativeShift::presentation::ConversionKind::Audio);
    } else if (value == L"video") {
        ApplyConversionKind(::NativeShift::presentation::ConversionKind::Video);
    } else {
        ApplyConversionKind(::NativeShift::presentation::ConversionKind::Image);
    }
}

void MainWindow::OpenSettings_Click(IInspectable const&,
                                    RoutedEventArgs const&) {
    NavigateTo(L"settings", true);
}

void MainWindow::QueueList_SelectionChanged(IInspectable const&,
                                            SelectionChangedEventArgs const&) {
    if (refreshing_view_) {
        return;
    }
    const auto selected = SelectedModelIndex();
    if (!selected.has_value()) {
        DetailsText().Text(
            L"Select a queue item to view progress, output path, codec, "
            L"warnings, or errors.");
        UpdateActionStates();
        return;
    }
    DetailsText().Text(view_model_.Rows()[*selected].DetailText());
    UpdateActionStates();
}

void MainWindow::Convert_Click(IInspectable const&, RoutedEventArgs const&) {
    if (active_kind_ == ::NativeShift::presentation::ConversionKind::Unknown) {
        ConvertStatus().Text(
            L"Add a supported file first. Its type will be detected "
            L"automatically.");
        return;
    }
    const auto format = SelectedComboText(ActiveOutputFormat());
    if (format.empty()) {
        ConvertStatus().Text(L"Select an output format before converting.");
        return;
    }
    const auto submitted = view_model_.Start(
        active_kind_,
        std::filesystem::path(DefaultOutputFolderSetting().Text().c_str()),
        format, BuildOptionsJson());
    if (submitted == 0) {
        ConvertStatus().Text(
            L"No ready files of this type are waiting. Add files first, or "
            L"choose their conversion type.");
        RefreshView();
        return;
    }
    ConvertStatus().Text(L"Started " + std::to_wstring(submitted) +
                         L" conversion(s). Progress is shown in Queue.");
    RefreshView();
    ShowQueuePage();
}

void MainWindow::Pause_Click(IInspectable const&, RoutedEventArgs const&) {
    view_model_.Pause();
    DetailsText().Text(
        L"Queue paused. Active conversions will finish; no new jobs start.");
    UpdateActionStates();
}

void MainWindow::Resume_Click(IInspectable const&, RoutedEventArgs const&) {
    view_model_.Resume();
    DetailsText().Text(L"Queue resumed.");
    UpdateActionStates();
}

void MainWindow::CancelSelected_Click(IInspectable const&,
                                      RoutedEventArgs const&) {
    if (const auto selected = SelectedModelIndex()) {
        view_model_.Cancel(*selected);
        DetailsText().Text(L"Cancellation requested.");
        RefreshView();
    } else {
        DetailsText().Text(L"Select an active job to cancel.");
    }
}

void MainWindow::CancelAll_Click(IInspectable const&, RoutedEventArgs const&) {
    view_model_.CancelAll();
    DetailsText().Text(L"Cancellation requested for all active jobs.");
    RefreshView();
}

void MainWindow::RetrySelected_Click(IInspectable const&,
                                     RoutedEventArgs const&) {
    if (const auto selected = SelectedModelIndex()) {
        if (!view_model_.Retry(*selected)) {
            DetailsText().Text(
                L"Only a finished, failed, or cancelled job can be retried.");
        } else {
            DetailsText().Text(L"Job queued again.");
        }
        RefreshView();
    } else {
        DetailsText().Text(L"Select a completed or failed job to retry.");
    }
}

void MainWindow::RemoveSelected_Click(IInspectable const&,
                                      RoutedEventArgs const&) {
    if (const auto selected = SelectedModelIndex()) {
        view_model_.Remove(*selected);
        RefreshView();
    } else {
        DetailsText().Text(L"Select a ready or finished job to remove.");
    }
}

void MainWindow::ClearCompleted_Click(IInspectable const&,
                                      RoutedEventArgs const&) {
    view_model_.ClearCompleted();
    DetailsText().Text(L"Completed jobs were cleared.");
    RefreshView();
}

void MainWindow::OpenOutput_Click(IInspectable const&, RoutedEventArgs const&) {
    std::filesystem::path path;
    if (const auto selected = SelectedModelIndex()) {
        path = view_model_.Rows()[*selected].output_path.parent_path();
    }
    if (path.empty()) {
        path =
            std::filesystem::path(DefaultOutputFolderSetting().Text().c_str());
    }
    std::error_code error;
    if (!path.empty() && std::filesystem::is_directory(path, error) && !error) {
        (void)::ShellExecuteW(WindowHandle(), L"open", path.c_str(), nullptr,
                              nullptr, SW_SHOWNORMAL);
    } else {
        DetailsText().Text(
            L"The selected job does not have an output folder yet.");
    }
}

void MainWindow::RefreshCapabilities_Click(IInspectable const&,
                                           RoutedEventArgs const&) {
    CapabilitiesText().Text(ReadCapabilities());
}

void MainWindow::PresetSelector_SelectionChanged(
    IInspectable const& sender, SelectionChangedEventArgs const&) {
    const auto selector = sender.try_as<ComboBox>();
    if (!selector) {
        return;
    }
    const auto id = SelectedComboText(selector);
    if (id == L"jpeg-high") {
        SelectOutputFormat(ImageOutputFormat(), L"jpeg");
        ImageQuality().Value(95);
    } else if (id == L"webp-balanced") {
        SelectOutputFormat(ImageOutputFormat(), L"webp");
        ImageQuality().Value(82);
    } else if (id == L"mp3-high") {
        SelectOutputFormat(AudioOutputFormat(), L"mp3");
        SelectComboTag(AudioCodec(), L"mp3");
        AudioBitrate().Value(320);
    } else if (id == L"opus-voice") {
        SelectOutputFormat(AudioOutputFormat(), L"opus");
        SelectComboTag(AudioCodec(), L"opus");
        AudioBitrate().Value(48);
    } else if (id == L"mp4-h264") {
        SelectOutputFormat(VideoOutputFormat(), L"mp4");
        SelectComboTag(VideoCodec(), L"h264");
        VideoQuality().Value(23);
    } else if (id == L"webm-vp9") {
        SelectOutputFormat(VideoOutputFormat(), L"webm");
        SelectComboTag(VideoCodec(), L"vp9");
        VideoQuality().Value(23);
    }
}

void MainWindow::SaveSettings_Click(IInspectable const&,
                                    RoutedEventArgs const&) {
    Windows::Data::Json::JsonObject document;
    document.Insert(L"default_output_folder",
                    Windows::Data::Json::JsonValue::CreateStringValue(
                        DefaultOutputFolderSetting().Text()));
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
    document.Insert(L"hardware_acceleration",
                    Windows::Data::Json::JsonValue::CreateStringValue(
                        SelectedComboText(HardwareSetting())));
    document.Insert(L"preserve_metadata",
                    Windows::Data::Json::JsonValue::CreateBooleanValue(
                        PreserveMetadataSetting().IsOn()));
    document.Insert(L"existing_file_policy",
                    Windows::Data::Json::JsonValue::CreateStringValue(
                        SelectedComboText(ExistingFilePolicySetting())));
    if (nativeshift_save_settings(document.Stringify().c_str())) {
        SettingsStatus().Text(
            L"Settings saved. Concurrency and logging changes apply after "
            L"restart.");
        const auto folder = DefaultOutputFolderSetting().Text();
        OutputLocationText().Text(
            folder.empty() ? L"Same folder as each source file" : folder);
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
    if (refreshing_view_) {
        return;
    }
    const auto selected_model = SelectedModelIndex();
    const auto state_filter = SelectedComboText(StateFilter());
    auto next_indices = view_model_.FilteredIndices(
        SearchBox().Text().c_str(), state_filter, QueueTypeFilterKind());
    std::vector<std::wstring> next_texts;
    next_texts.reserve(next_indices.size());
    for (const auto model : next_indices) {
        next_texts.push_back(view_model_.Rows()[model].DisplayText());
    }

    refreshing_view_ = true;
    int selected_display = -1;
    for (std::size_t display = 0; display < next_indices.size(); ++display) {
        const auto model = next_indices[display];
        if (selected_model.has_value() && *selected_model == model) {
            selected_display = static_cast<int>(display);
        }
    }

    const bool structure_changed = next_indices != visible_indices_;
    if (structure_changed) {
        QueueList().Items().Clear();
        for (const auto& text : next_texts) {
            QueueList().Items().Append(box_value(hstring(text)));
        }
        QueueList().SelectedIndex(selected_display);
    } else {
        for (std::size_t index = 0; index < next_texts.size(); ++index) {
            if (index >= queue_item_texts_.size() ||
                next_texts[index] != queue_item_texts_[index]) {
                QueueList().Items().SetAt(
                    static_cast<std::uint32_t>(index),
                    box_value(hstring(next_texts[index])));
            }
        }
    }
    visible_indices_ = std::move(next_indices);
    queue_item_texts_ = std::move(next_texts);
    OverallProgress().Value(view_model_.OverallProgress() * 100.0);
    const bool has_active_job =
        std::ranges::any_of(view_model_.Rows(), [](const auto& row) {
            return row.id != 0 && !row.IsTerminal();
        });
    QueueActivity().IsActive(has_active_job);
    QueueActivityPanel().Visibility(has_active_job ? Visibility::Visible
                                                   : Visibility::Collapsed);

    const auto job_exists = [this](const std::uint64_t id) {
        return std::ranges::any_of(
            view_model_.Rows(), [id](const auto& row) { return row.id == id; });
    };
    std::erase_if(pinned_jobs_, [&](const auto id) { return !job_exists(id); });
    std::erase_if(archived_jobs_,
                  [&](const auto id) { return !job_exists(id); });

    std::vector<std::size_t> next_sidebar_indices;
    std::vector<std::wstring> next_sidebar_texts;
    const auto append_sidebar_jobs = [&](const bool pinned) {
        for (std::size_t offset = 0; offset < view_model_.Rows().size() &&
                                     next_sidebar_indices.size() < 8;
             ++offset) {
            const auto index = view_model_.Rows().size() - 1 - offset;
            const auto& row = view_model_.Rows()[index];
            if (row.id == 0 || archived_jobs_.contains(row.id) ||
                pinned_jobs_.contains(row.id) != pinned) {
                continue;
            }
            auto state = row.state;
            std::ranges::replace(state, L'_', L' ');
            next_sidebar_indices.push_back(index);
            next_sidebar_texts.push_back(std::to_wstring(row.id) + L"|" +
                                         row.input_path.filename().native() +
                                         L"|" + state +
                                         (pinned ? L"|pinned" : L"|recent"));
        }
    };
    append_sidebar_jobs(true);
    append_sidebar_jobs(false);
    if (next_sidebar_texts != sidebar_job_texts_) {
        SidebarJobsList().Items().Clear();
        for (const auto index : next_sidebar_indices) {
            const auto& row = view_model_.Rows()[index];
            SidebarJobsList().Items().Append(
                BuildSidebarJobItem(row, pinned_jobs_.contains(row.id)));
        }
        sidebar_job_texts_ = next_sidebar_texts;
    }
    sidebar_job_indices_ = std::move(next_sidebar_indices);
    SidebarJobsList().Visibility(sidebar_job_indices_.empty()
                                     ? Visibility::Collapsed
                                     : Visibility::Visible);
    SidebarJobsEmpty().Visibility(sidebar_job_indices_.empty()
                                      ? Visibility::Visible
                                      : Visibility::Collapsed);
    refreshing_view_ = false;
    UpdateAddedFiles();
    UpdateActionStates();
}

void MainWindow::AddPaths(const std::vector<std::filesystem::path>& paths) {
    const auto before = view_model_.Rows().size();
    view_model_.AddFiles(paths);
    const auto added = view_model_.Rows().size() - before;
    const auto unsupported = std::ranges::count_if(
        view_model_.Rows().begin() + static_cast<std::ptrdiff_t>(before),
        view_model_.Rows().end(), [](const auto& row) {
            return row.kind ==
                   ::NativeShift::presentation::ConversionKind::Unknown;
        });
    const auto first_supported = std::find_if(
        view_model_.Rows().begin() + static_cast<std::ptrdiff_t>(before),
        view_model_.Rows().end(), [](const auto& row) {
            return row.kind !=
                   ::NativeShift::presentation::ConversionKind::Unknown;
        });
    if (first_supported != view_model_.Rows().end()) {
        ApplyConversionKind(first_supported->kind);
    }
    ConvertStatus().Text(
        L"Added " + std::to_wstring(added) + L" file(s). " +
        (unsupported > 0
             ? std::to_wstring(unsupported) +
                   L" could not be identified as a supported format."
             : L"Each file is ready in its matching conversion queue."));
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
    ComboBox preset_selector = ImagePresetSelector();
    json << L"{";
    if (active_kind_ == ::NativeShift::presentation::ConversionKind::Image) {
        json << LR"("image_quality":)"
             << static_cast<int>(ImageQuality().Value());
        preset_selector = ImagePresetSelector();
        if (!std::isnan(ImageWidth().Value())) {
            json << LR"(,"width":)"
                 << static_cast<std::uint32_t>(ImageWidth().Value());
        }
        if (!std::isnan(ImageHeight().Value())) {
            json << LR"(,"height":)"
                 << static_cast<std::uint32_t>(ImageHeight().Value());
        }
    } else if (active_kind_ ==
               ::NativeShift::presentation::ConversionKind::Audio) {
        json << LR"("audio_bitrate_kbps":)"
             << static_cast<int>(AudioBitrate().Value())
             << LR"(,"audio_codec":")" << SelectedComboText(AudioCodec())
             << L"\"";
        preset_selector = AudioPresetSelector();
    } else {
        json << LR"("video_quality":)"
             << static_cast<int>(VideoQuality().Value())
             << LR"(,"video_codec":")" << SelectedComboText(VideoCodec())
             << L"\"";
        preset_selector = VideoPresetSelector();
        if (!std::isnan(VideoWidth().Value())) {
            json << LR"(,"width":)"
                 << static_cast<std::uint32_t>(VideoWidth().Value());
        }
        if (!std::isnan(VideoHeight().Value())) {
            json << LR"(,"height":)"
                 << static_cast<std::uint32_t>(VideoHeight().Value());
        }
    }
    json << LR"(,"preserve_metadata":)"
         << (PreserveMetadataSetting().IsOn() ? L"true" : L"false");
    const auto preset = preset_selector.SelectedItem().try_as<ComboBoxItem>();
    if (preset && preset.Tag()) {
        const auto id = unbox_value_or<hstring>(preset.Tag(), L"");
        if (!id.empty()) {
            json << LR"(,"preset_id":")" << id.c_str() << LR"(")";
        }
    }
    auto hardware = SelectedComboText(HardwareSetting());
    if (hardware == L"prefer_hardware") {
        hardware = L"prefer";
    } else if (hardware == L"disabled") {
        hardware = L"software";
    }
    json << LR"(,"hardware":")" << hardware << LR"(","conflict":")"
         << SelectedComboText(ExistingFilePolicySetting()) << LR"("})";
    return json.str();
}

void MainWindow::LoadSettings() {
    try {
        const auto json = ReadBridgeValue(nativeshift_settings_json);
        if (json.empty()) {
            return;
        }
        const auto settings = Windows::Data::Json::JsonObject::Parse(json);
        DefaultOutputFolderSetting().Text(
            settings.GetNamedString(L"default_output_folder", L""));
        const auto folder = DefaultOutputFolderSetting().Text();
        OutputLocationText().Text(
            folder.empty() ? L"Same folder as each source file" : folder);
        MaximumJobsSetting().Value(
            settings.GetNamedNumber(L"maximum_concurrent_conversions", 2.0));
        NotificationsSetting().IsOn(
            settings.GetNamedBoolean(L"notifications_enabled", true));
        SelectComboTag(ThemeSetting(),
                       settings.GetNamedString(L"theme", L"system").c_str());
        SelectComboTag(
            LoggingSetting(),
            settings.GetNamedString(L"logging_level", L"information").c_str());
        SelectComboTag(
            HardwareSetting(),
            settings.GetNamedString(L"hardware_acceleration", L"auto").c_str());
        PreserveMetadataSetting().IsOn(
            settings.GetNamedBoolean(L"preserve_metadata", false));
        SelectComboTag(
            ExistingFilePolicySetting(),
            settings.GetNamedString(L"existing_file_policy", L"unique")
                .c_str());
        SettingsStatus().Text(settings.GetNamedString(L"warning", L""));
    } catch (const hresult_error& error) {
        SettingsStatus().Text(error.message());
    }
}

void MainWindow::SelectOutputFormat(const ComboBox combo,
                                    const std::wstring_view value) {
    for (std::uint32_t index = 0; index < combo.Items().Size(); ++index) {
        const auto item = combo.Items().GetAt(index).try_as<ComboBoxItem>();
        if (item && unbox_value_or<hstring>(item.Content(), L"") == value) {
            combo.SelectedIndex(static_cast<int>(index));
            return;
        }
    }
}

void MainWindow::ApplyConversionKind(
    const ::NativeShift::presentation::ConversionKind kind) {
    active_kind_ = kind;
    const bool image =
        kind == ::NativeShift::presentation::ConversionKind::Image;
    const bool audio =
        kind == ::NativeShift::presentation::ConversionKind::Audio;
    const bool video =
        kind == ::NativeShift::presentation::ConversionKind::Video;

    ImageOptionsPanel().Visibility(image ? Visibility::Visible
                                         : Visibility::Collapsed);
    AudioOptionsPanel().Visibility(audio ? Visibility::Visible
                                         : Visibility::Collapsed);
    VideoOptionsPanel().Visibility(video ? Visibility::Visible
                                         : Visibility::Collapsed);
    OptionsEmptyText().Visibility(
        image || audio || video ? Visibility::Collapsed : Visibility::Visible);
    ModeTitle().Text(L"Files to start");

    if (image) {
        ModeDescription().Text(L"PNG, JPEG, WebP, BMP, or TIFF");
        Automation::AutomationProperties::SetName(ConvertButton(),
                                                  L"Convert images");
        ToolTipService::SetToolTip(ConvertButton(),
                                   box_value(L"Convert images"));
    } else if (audio) {
        ModeDescription().Text(L"MP3, WAV, FLAC, AAC, M4A, Ogg, or Opus");
        Automation::AutomationProperties::SetName(ConvertButton(),
                                                  L"Convert audio");
        ToolTipService::SetToolTip(ConvertButton(),
                                   box_value(L"Convert audio"));
    } else if (video) {
        ModeDescription().Text(L"MP4, MKV, MOV, AVI, or WebM");
        Automation::AutomationProperties::SetName(ConvertButton(),
                                                  L"Convert video");
        ToolTipService::SetToolTip(ConvertButton(),
                                   box_value(L"Convert video"));
    } else {
        ModeDescription().Text(L"Images, audio, and video");
        Automation::AutomationProperties::SetName(ConvertButton(), L"Convert");
        ToolTipService::SetToolTip(ConvertButton(),
                                   box_value(L"Add a file first"));
    }
    UpdateAddedFiles();
}

::NativeShift::presentation::ConversionKind MainWindow::QueueTypeFilterKind() {
    const auto value = SelectedComboText(QueueTypeFilter());
    if (value == L"image") {
        return ::NativeShift::presentation::ConversionKind::Image;
    }
    if (value == L"audio") {
        return ::NativeShift::presentation::ConversionKind::Audio;
    }
    if (value == L"video") {
        return ::NativeShift::presentation::ConversionKind::Video;
    }
    return ::NativeShift::presentation::ConversionKind::All;
}

ComboBox MainWindow::ActiveOutputFormat() {
    if (active_kind_ == ::NativeShift::presentation::ConversionKind::Audio) {
        return AudioOutputFormat();
    }
    if (active_kind_ == ::NativeShift::presentation::ConversionKind::Video) {
        return VideoOutputFormat();
    }
    return ImageOutputFormat();
}

void MainWindow::ShowQueuePage() {
    NavigateTo(L"queue", true);
    SearchBox().Focus(FocusState::Programmatic);
}

void MainWindow::UpdateAddedFiles() {
    std::vector<std::wstring> next_texts;
    std::size_t image_count{};
    std::size_t audio_count{};
    std::size_t video_count{};
    std::filesystem::path preview_path;
    for (const auto& row : view_model_.Rows()) {
        if (row.id != 0) {
            continue;
        }
        if (row.kind == ::NativeShift::presentation::ConversionKind::Image) {
            ++image_count;
            if (preview_path.empty()) {
                preview_path = row.input_path;
            }
        } else if (row.kind ==
                   ::NativeShift::presentation::ConversionKind::Audio) {
            ++audio_count;
        } else if (row.kind ==
                   ::NativeShift::presentation::ConversionKind::Video) {
            ++video_count;
        }
        std::wstring text = row.input_path.filename().native();
        text += L"  |  ";
        text += row.input_format;
        text += L"  |  ";
        text += row.state;
        if (!row.output_format.empty()) {
            text += L" -> ";
            text += row.output_format;
        }
        next_texts.push_back(std::move(text));
    }

    const bool files_changed = next_texts != added_file_texts_;
    if (files_changed) {
        AddedFilesList().Items().Clear();
        for (const auto& text : next_texts) {
            AddedFilesList().Items().Append(box_value(hstring(text)));
        }
        added_file_texts_ = next_texts;
    }
    AddedFilesCard().Visibility(next_texts.empty() ? Visibility::Collapsed
                                                   : Visibility::Visible);
    AddedFilesHeading().Text(
        next_texts.empty()
            ? L"Files added"
            : L"Files added (" + std::to_wstring(next_texts.size()) + L")");

    const auto update_kind = [](const Button& button, const std::size_t count,
                                const std::wstring_view label) {
        button.Visibility(count == 0 ? Visibility::Collapsed
                                     : Visibility::Visible);
        button.Content(box_value(std::wstring(label) + L" (" +
                                 std::to_wstring(count) + L")"));
    };
    update_kind(DetectedImagesButton(), image_count, L"Images");
    update_kind(DetectedAudioButton(), audio_count, L"Audio");
    update_kind(DetectedVideoButton(), video_count, L"Video");
    const bool has_supported = image_count + audio_count + video_count > 0;
    DetectedTypePanel().Visibility(has_supported ? Visibility::Visible
                                                 : Visibility::Collapsed);

    if (preview_path.empty()) {
        ++preview_request_;
        ImagePreview().Source(nullptr);
        ImagePreviewBorder().Visibility(Visibility::Collapsed);
    } else if (files_changed ||
               ImagePreviewBorder().Visibility() != Visibility::Visible) {
        ImagePreviewBorder().Visibility(Visibility::Visible);
        ImagePreviewPlaceholder().Visibility(Visibility::Visible);
        LoadImagePreview(preview_path, ++preview_request_);
    }

    const auto selected_background = RootLayout()
                                         .Resources()
                                         .Lookup(box_value(L"PrimaryTextBrush"))
                                         .as<Media::Brush>();
    const auto selected_foreground = RootLayout()
                                         .Resources()
                                         .Lookup(box_value(L"DarkTextBrush"))
                                         .as<Media::Brush>();
    const auto normal_background = RootLayout()
                                       .Resources()
                                       .Lookup(box_value(L"TransparentBrush"))
                                       .as<Media::Brush>();
    const auto normal_foreground = RootLayout()
                                       .Resources()
                                       .Lookup(box_value(L"PrimaryTextBrush"))
                                       .as<Media::Brush>();
    const auto select =
        [&](const Button& button,
            const ::NativeShift::presentation::ConversionKind kind) {
            const bool selected = active_kind_ == kind;
            button.Background(selected ? selected_background
                                       : normal_background);
            button.Foreground(selected ? selected_foreground
                                       : normal_foreground);
        };
    select(DetectedImagesButton(),
           ::NativeShift::presentation::ConversionKind::Image);
    select(DetectedAudioButton(),
           ::NativeShift::presentation::ConversionKind::Audio);
    select(DetectedVideoButton(),
           ::NativeShift::presentation::ConversionKind::Video);

    const std::size_t active_count =
        active_kind_ == ::NativeShift::presentation::ConversionKind::Image
            ? image_count
        : active_kind_ == ::NativeShift::presentation::ConversionKind::Audio
            ? audio_count
        : active_kind_ == ::NativeShift::presentation::ConversionKind::Video
            ? video_count
            : 0;
    ConvertButton().IsEnabled(active_count > 0);
    OptionsButton().IsEnabled(active_count > 0);
}

void MainWindow::ResetCurrentSession(const std::wstring_view status) {
    view_model_.ClearStaged();
    ConvertPage().IsPaneOpen(false);
    ApplyConversionKind(::NativeShift::presentation::ConversionKind::Unknown);
    ConvertStatus().Text(status);
    RefreshView();
}

fire_and_forget MainWindow::LoadImagePreview(std::filesystem::path path,
                                             const std::uint64_t request) {
    [[maybe_unused]] const auto lifetime = get_strong();
    try {
        const auto file =
            co_await StorageFile::GetFileFromPathAsync(path.native());
        const auto stream = co_await file.OpenAsync(FileAccessMode::Read);
        Media::Imaging::BitmapImage bitmap;
        co_await bitmap.SetSourceAsync(stream);
        if (request != preview_request_) {
            co_return;
        }
        ImagePreview().Source(bitmap);
        ImagePreviewPlaceholder().Visibility(Visibility::Collapsed);
    } catch (const hresult_error&) {
        if (request == preview_request_) {
            ImagePreviewPlaceholder().Visibility(Visibility::Visible);
        }
    }
}

void MainWindow::SelectNavigationTag(const std::wstring_view tag) {
    NavigateTo(tag, false);
}

void MainWindow::UpdateNavigationButtons() {
    const bool can_back =
        !navigation_history_.empty() && navigation_position_ > 0;
    const bool can_forward =
        !navigation_history_.empty() &&
        navigation_position_ + 1 < navigation_history_.size();
    BackButton().IsHitTestVisible(can_back);
    BackButton().Opacity(can_back ? 1.0 : 0.42);
    ForwardButton().IsHitTestVisible(can_forward);
    ForwardButton().Opacity(can_forward ? 1.0 : 0.42);
}

void MainWindow::UpdateSidebarSelection(const std::wstring_view tag) {
    const auto selected = RootLayout()
                              .Resources()
                              .Lookup(box_value(L"SidebarSelectedBrush"))
                              .as<Media::Brush>();
    const auto transparent = RootLayout()
                                 .Resources()
                                 .Lookup(box_value(L"TransparentBrush"))
                                 .as<Media::Brush>();
    const auto apply = [&](const Button& button,
                           const std::wstring_view button_tag) {
        button.Background(button_tag == tag ? selected : transparent);
    };
    apply(ConvertNavButton(), L"convert");
    apply(QueueNavButton(), L"queue");
    apply(SettingsNavButton(), L"settings");
    apply(CapabilitiesNavButton(), L"capabilities");
    apply(AboutNavButton(), L"about");
}

void MainWindow::SetSidebarOpen(const bool open) {
    const auto animation_key =
        open ? L"SidebarOpenAnimation" : L"SidebarCloseAnimation";
    if (const auto storyboard = RootLayout()
                                    .Resources()
                                    .Lookup(box_value(animation_key))
                                    .try_as<Media::Animation::Storyboard>()) {
        storyboard.Begin();
    }
    sidebar_open_ = open;
    SidebarPane().Visibility(open ? Visibility::Visible
                                  : Visibility::Collapsed);
    SidebarColumn().Width(Microsoft::UI::Xaml::GridLength{open ? 250.0 : 0.0,
                                                          GridUnitType::Pixel});
}

void MainWindow::UpdateActionStates() {
    const auto selected = SelectedModelIndex();
    const bool has_jobs = !view_model_.Rows().empty();
    bool can_cancel = false;
    bool can_retry = false;
    bool can_remove = false;
    bool can_open = false;
    if (selected.has_value()) {
        const auto& row = view_model_.Rows()[*selected];
        can_cancel = row.id != 0 && !row.IsTerminal();
        can_retry = row.id != 0 && row.IsTerminal();
        can_remove = row.id == 0 || row.IsTerminal();
        std::error_code error;
        can_open = !row.output_path.empty() &&
                   std::filesystem::is_directory(row.output_path.parent_path(),
                                                 error) &&
                   !error;
    }
    CancelSelectedButton().IsEnabled(can_cancel);
    RetrySelectedButton().IsEnabled(can_retry);
    RemoveSelectedButton().IsEnabled(can_remove);
    OpenOutputButton().IsEnabled(can_open);
    PauseQueueButton().IsEnabled(has_jobs && !view_model_.IsPaused());
    ResumeQueueButton().IsEnabled(has_jobs && view_model_.IsPaused());
    CancelAllButton().IsEnabled(has_jobs);
    ClearCompletedButton().IsEnabled(std::ranges::any_of(
        view_model_.Rows(), [](const auto& row) { return row.IsTerminal(); }));
}

} // namespace winrt::NativeShift::implementation
