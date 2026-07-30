#pragma once

#include "App.xaml.g.h"

namespace winrt::NativeShift::implementation {

struct App : AppT<App> {
    App();
    void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);

  private:
    winrt::Microsoft::UI::Xaml::Window window_{nullptr};
};

} // namespace winrt::NativeShift::implementation
