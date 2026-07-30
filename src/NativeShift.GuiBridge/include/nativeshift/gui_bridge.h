#pragma once

#include <cstddef>
#include <cstdint>

#ifdef _WIN32
#ifdef NATIVESHIFT_GUI_BRIDGE_EXPORTS
#define NATIVESHIFT_GUI_API __declspec(dllexport)
#else
#define NATIVESHIFT_GUI_API __declspec(dllimport)
#endif
#else
#define NATIVESHIFT_GUI_API
#endif

extern "C" {

NATIVESHIFT_GUI_API std::uint64_t
nativeshift_submit(const wchar_t* input_path, const wchar_t* output_path,
                   const wchar_t* output_format, const wchar_t* options_json);
NATIVESHIFT_GUI_API bool nativeshift_cancel(std::uint64_t job_id);
NATIVESHIFT_GUI_API std::uint64_t nativeshift_retry(std::uint64_t job_id);
NATIVESHIFT_GUI_API void nativeshift_cancel_all();
NATIVESHIFT_GUI_API void nativeshift_pause_queue();
NATIVESHIFT_GUI_API void nativeshift_resume_queue();
NATIVESHIFT_GUI_API bool nativeshift_remove(std::uint64_t job_id);
NATIVESHIFT_GUI_API std::size_t
nativeshift_job_json(std::uint64_t job_id, wchar_t* destination,
                     std::size_t destination_size);
NATIVESHIFT_GUI_API std::size_t
nativeshift_queue_json(wchar_t* destination, std::size_t destination_size);
NATIVESHIFT_GUI_API std::size_t
nativeshift_capabilities_json(wchar_t* destination,
                              std::size_t destination_size);
NATIVESHIFT_GUI_API std::size_t
nativeshift_presets_json(wchar_t* destination, std::size_t destination_size);
NATIVESHIFT_GUI_API std::size_t
nativeshift_settings_json(wchar_t* destination, std::size_t destination_size);
NATIVESHIFT_GUI_API bool
nativeshift_save_settings(const wchar_t* settings_json);
NATIVESHIFT_GUI_API bool nativeshift_reset_settings();
NATIVESHIFT_GUI_API std::size_t
nativeshift_diagnostic_summary(wchar_t* destination,
                               std::size_t destination_size);
NATIVESHIFT_GUI_API std::size_t
nativeshift_log_folder(wchar_t* destination, std::size_t destination_size);
NATIVESHIFT_GUI_API std::size_t
nativeshift_last_error(wchar_t* destination, std::size_t destination_size);
}
