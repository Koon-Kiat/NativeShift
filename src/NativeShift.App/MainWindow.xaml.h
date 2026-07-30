#pragma once

#include "MainWindow.g.h"
#include "QueueViewModel.h"

#include <filesystem>
#include <vector>

namespace winrt::NativeShift::implementation {

struct MainWindow : MainWindowT<MainWindow> {
    MainWindow();

    void Navigation_SelectionChanged(
        Microsoft::UI::Xaml::Controls::NavigationView const&,
        Microsoft::UI::Xaml::Controls::
            NavigationViewSelectionChangedEventArgs const& event);
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
    void QueueList_SelectionChanged(
        IInspectable const&,
        Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
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
    void SelectOutputFormat(std::wstring_view value);

    ::NativeShift::presentation::QueueViewModel view_model_;
    std::vector<std::size_t> visible_indices_;
    Microsoft::UI::Xaml::DispatcherTimer refresh_timer_;
};

} // namespace winrt::NativeShift::implementation

namespace winrt::NativeShift::factory_implementation {

struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};

} // namespace winrt::NativeShift::factory_implementation
