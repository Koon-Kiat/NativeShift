#pragma once

#include "MainWindow.g.h"
#include "QueueViewModel.h"

#include <filesystem>
#include <optional>
#include <unordered_set>
#include <vector>

namespace winrt::NativeShift::implementation {

struct MainWindow : MainWindowT<MainWindow> {
    MainWindow();

    void ToggleSidebar_Click(IInspectable const&,
                             Microsoft::UI::Xaml::RoutedEventArgs const&);
    void SidebarNavigation_Click(IInspectable const&,
                                 Microsoft::UI::Xaml::RoutedEventArgs const&);
    void MenuNavigate_Click(IInspectable const&,
                            Microsoft::UI::Xaml::RoutedEventArgs const&);
    void Back_Click(IInspectable const&,
                    Microsoft::UI::Xaml::RoutedEventArgs const&);
    void Forward_Click(IInspectable const&,
                       Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OpenQueue_Click(IInspectable const&,
                         Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ToggleOptions_Click(IInspectable const&,
                             Microsoft::UI::Xaml::RoutedEventArgs const&);
    void NewConversion_Click(IInspectable const&,
                             Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ClearCurrentSession_Click(IInspectable const&,
                                   Microsoft::UI::Xaml::RoutedEventArgs const&);
    void SidebarJob_ItemClick(
        IInspectable const&,
        Microsoft::UI::Xaml::Controls::ItemClickEventArgs const& event);
    void
    SidebarJobAction_Click(IInspectable const&,
                           Microsoft::UI::Xaml::RoutedEventArgs const& event);
    winrt::fire_and_forget
    AddFiles_Click(IInspectable const&,
                   Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget
    AddFolder_Click(IInspectable const&,
                    Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget
    BrowseOutput_Click(IInspectable const&,
                       Microsoft::UI::Xaml::RoutedEventArgs const&);
    void DropArea_DragOver(IInspectable const&,
                           Microsoft::UI::Xaml::DragEventArgs const& event);
    winrt::fire_and_forget
    DropArea_Drop(IInspectable const&,
                  Microsoft::UI::Xaml::DragEventArgs const& event);
    void SearchBox_TextChanged(
        IInspectable const&,
        Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&);
    void StateFilter_SelectionChanged(
        IInspectable const&,
        Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void QueueTypeFilter_SelectionChanged(
        IInspectable const&,
        Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void QueueList_SelectionChanged(
        IInspectable const&,
        Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void DetectedType_Click(IInspectable const&,
                            Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OpenSettings_Click(IInspectable const&,
                            Microsoft::UI::Xaml::RoutedEventArgs const&);
    void Convert_Click(IInspectable const&,
                       Microsoft::UI::Xaml::RoutedEventArgs const&);
    void Pause_Click(IInspectable const&,
                     Microsoft::UI::Xaml::RoutedEventArgs const&);
    void Resume_Click(IInspectable const&,
                      Microsoft::UI::Xaml::RoutedEventArgs const&);
    void CancelSelected_Click(IInspectable const&,
                              Microsoft::UI::Xaml::RoutedEventArgs const&);
    void CancelAll_Click(IInspectable const&,
                         Microsoft::UI::Xaml::RoutedEventArgs const&);
    void RetrySelected_Click(IInspectable const&,
                             Microsoft::UI::Xaml::RoutedEventArgs const&);
    void RemoveSelected_Click(IInspectable const&,
                              Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ClearCompleted_Click(IInspectable const&,
                              Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OpenOutput_Click(IInspectable const&,
                          Microsoft::UI::Xaml::RoutedEventArgs const&);
    void RefreshCapabilities_Click(IInspectable const&,
                                   Microsoft::UI::Xaml::RoutedEventArgs const&);
    void PresetSelector_SelectionChanged(
        IInspectable const&,
        Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void SaveSettings_Click(IInspectable const&,
                            Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ResetSettings_Click(IInspectable const&,
                             Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OpenLogs_Click(IInspectable const&,
                        Microsoft::UI::Xaml::RoutedEventArgs const&);
    void CopyDiagnostics_Click(IInspectable const&,
                               Microsoft::UI::Xaml::RoutedEventArgs const&);

  private:
    [[nodiscard]] HWND WindowHandle() const;
    void RefreshView();
    void AddPaths(const std::vector<std::filesystem::path>& paths);
    [[nodiscard]] std::optional<std::size_t> SelectedModelIndex();
    [[nodiscard]] std::wstring
    SelectedComboText(Microsoft::UI::Xaml::Controls::ComboBox combo);
    [[nodiscard]] std::wstring BuildOptionsJson();
    void LoadSettings();
    void ApplyConversionKind(::NativeShift::presentation::ConversionKind kind);
    [[nodiscard]] ::NativeShift::presentation::ConversionKind
    QueueTypeFilterKind();
    [[nodiscard]] Microsoft::UI::Xaml::Controls::ComboBox ActiveOutputFormat();
    void SelectOutputFormat(Microsoft::UI::Xaml::Controls::ComboBox combo,
                            std::wstring_view value);
    void ShowQueuePage();
    void UpdateActionStates();
    void UpdateAddedFiles();
    void SelectNavigationTag(std::wstring_view tag);
    void UpdateNavigationButtons();
    void NavigateTo(std::wstring_view tag, bool record_history);
    void UpdateSidebarSelection(std::wstring_view tag);
    void SetSidebarOpen(bool open);
    void ResetCurrentSession(std::wstring_view status);
    [[nodiscard]] Microsoft::UI::Xaml::Controls::Grid
    BuildSidebarJobItem(::NativeShift::presentation::QueueRow const& row,
                        bool pinned);
    winrt::fire_and_forget LoadImagePreview(std::filesystem::path path,
                                            std::uint64_t request);

    ::NativeShift::presentation::QueueViewModel view_model_;
    ::NativeShift::presentation::ConversionKind active_kind_{
        ::NativeShift::presentation::ConversionKind::Unknown};
    std::vector<std::size_t> visible_indices_;
    std::vector<std::wstring> queue_item_texts_;
    std::vector<std::wstring> added_file_texts_;
    std::vector<std::size_t> sidebar_job_indices_;
    std::vector<std::wstring> sidebar_job_texts_;
    std::unordered_set<std::uint64_t> pinned_jobs_;
    std::unordered_set<std::uint64_t> archived_jobs_;
    std::vector<std::wstring> navigation_history_;
    std::size_t navigation_position_{};
    bool refreshing_view_{false};
    bool sidebar_open_{true};
    std::uint64_t preview_request_{};
    Microsoft::UI::Xaml::DispatcherTimer refresh_timer_;
};

} // namespace winrt::NativeShift::implementation

namespace winrt::NativeShift::factory_implementation {

struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};

} // namespace winrt::NativeShift::factory_implementation
