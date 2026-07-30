#include "pch.h"

#include "App.xaml.h"
#include "MainWindow.xaml.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::NativeShift::implementation {

App::App() {
    InitializeComponent();
#if defined _DEBUG &&                                                          \
    !defined DISABLE_XAML_GENERATED_BREAK_ON_UNHANDLED_EXCEPTION
    UnhandledException(
        [](IInspectable const&, UnhandledExceptionEventArgs const& event) {
            if (::IsDebuggerPresent() != FALSE) {
                const auto message = event.Message();
                (void)message;
                __debugbreak();
            }
        });
#endif
}

void App::OnLaunched(LaunchActivatedEventArgs const&) {
    window_ = make<MainWindow>();
    window_.Activate();
}

} // namespace winrt::NativeShift::implementation
