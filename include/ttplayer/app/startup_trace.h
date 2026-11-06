#pragma once
// Optional, zero-impact startup timing.
//
// Set the TTPLAYER_STARTUP_TRACE environment variable to write a phase log to
// %TEMP%\ttplayer_startup.log. When the variable is absent every call costs one
// (cached) GetEnvironmentVariableW and no file I/O. Times are milliseconds
// since the first mark in the process, so a phase's cost is the delta between
// two consecutive lines.
#include <windows.h>

#include <cstdio>
#include <string>

namespace ttplayer::app::startup_trace {

inline LARGE_INTEGER& Frequency() {
    static LARGE_INTEGER value = [] {
        LARGE_INTEGER v{};
        QueryPerformanceFrequency(&v);
        return v;
    }();
    return value;
}

inline LARGE_INTEGER& Origin() {
    static LARGE_INTEGER value = [] {
        LARGE_INTEGER v{};
        QueryPerformanceCounter(&v);
        return v;
    }();
    return value;
}

inline bool Enabled() {
    static const bool value =
        GetEnvironmentVariableW(L"TTPLAYER_STARTUP_TRACE", nullptr, 0) != 0;
    return value;
}

inline std::wstring LogPath() {
    wchar_t temp[MAX_PATH]{};
    const DWORD length = GetTempPathW(MAX_PATH, temp);
    if (length == 0 || length >= MAX_PATH) return L"ttplayer_startup.log";
    return std::wstring(temp, length) + L"ttplayer_startup.log";
}

inline void Reset() {
    (void)Frequency();
    (void)Origin();
    if (!Enabled()) return;
    FILE* file = nullptr;
    if (_wfopen_s(&file, LogPath().c_str(), L"wb") == 0 && file)
        std::fclose(file);
}

inline void Mark(const char* label) {
    (void)Origin();
    if (!Enabled()) return;
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const double ms =
        static_cast<double>(now.QuadPart - Origin().QuadPart) * 1000.0 /
        static_cast<double>(Frequency().QuadPart ? Frequency().QuadPart : 1);
    FILE* file = nullptr;
    if (_wfopen_s(&file, LogPath().c_str(), L"ab") == 0 && file) {
        std::fprintf(file, "%9.2f  %s\n", ms, label);
        std::fclose(file);
    }
}

}  // namespace ttplayer::app::startup_trace

#define TT_STARTUP_TRACE(label) ::ttplayer::app::startup_trace::Mark(label)
#define TT_STARTUP_TRACE_RESET() ::ttplayer::app::startup_trace::Reset()
