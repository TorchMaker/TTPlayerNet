#pragma once

// 内容插件（直播 / 听书 / TTS）公共加载工具（仅宿主侧使用）。
//
// 统一约定：插件放在 <exe>\AddIn\，文件名形如 ttp_l_*.dll / ttp_b_*.dll /
// ttp_t_*.dll；工厂函数用 GetProcAddress 取名，x86 下同时尝试 __stdcall
// 修饰名（与 video_protocol_registry 的做法一致）。
//
// Win7 约束：只用 LoadLibraryW / GetProcAddress，不产生启动导入；插件缺失
// 或版本不符时记录日志并跳过，绝不阻塞宿主。

#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include <windows.h>

#include "ttplayer/core/text.h"

namespace ttplayer::plugins {

inline std::filesystem::path ExeDirectory() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return {};
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
}

// 插件日志：live → <exe>\video\live.log；book → <exe>\book\book.log；
// tts → <exe>\book\tts.log。写失败静默（诊断用途，不影响主流程）。
inline void AppendContentLog(const std::wstring& category,
                             const std::string& text) {
    try {
        std::filesystem::path path = ExeDirectory();
        if (path.empty()) return;
        if (category == L"live") {
            path /= L"video";
            path /= L"live.log";
        } else {
            path /= L"book";
            path /= (category == L"tts" ? L"tts.log" : L"book.log");
        }
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_s(&local, &now);
        char stamp[32]{};
        std::strftime(stamp, sizeof(stamp), "%m-%d %H:%M:%S", &local);
        FILE* file = nullptr;
        if (_wfopen_s(&file, path.c_str(), L"ab") != 0 || !file) return;
        std::fprintf(file, "[%s] %s\r\n", stamp, text.c_str());
        std::fclose(file);
    } catch (...) {
    }
}

// 扫描 <exe>\AddIn\<prefix>*.dll（prefix 例如 L"ttp_l_"）。
inline std::vector<std::filesystem::path> FindContentPlugins(
    const wchar_t* prefix) {
    std::vector<std::filesystem::path> result;
    std::error_code ec;
    std::filesystem::path dir = ExeDirectory();
    if (dir.empty()) return result;
    dir /= L"AddIn";
    if (!std::filesystem::is_directory(dir, ec)) return result;
    const size_t prefix_len = wcslen(prefix);
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const std::wstring name = entry.path().filename().wstring();
        if (_wcsnicmp(name.c_str(), prefix, prefix_len) != 0) continue;
        if (_wcsicmp(entry.path().extension().c_str(), L".dll") != 0) continue;
        result.push_back(entry.path());
    }
    return result;
}

// 取工厂函数：先试未修饰名，再试 x86 __stdcall 修饰名（_name@N）。
inline FARPROC GetPluginFactory(HMODULE module, const char* name) {
    if (!module || !name) return nullptr;
    FARPROC proc = GetProcAddress(module, name);
    if (proc) return proc;
    // x86 __stdcall 的修饰名：_ttpGetXxxSource@4。
    std::string decorated = "_";
    decorated += name;
    decorated += "@4";
    return GetProcAddress(module, decorated.c_str());
}

inline void SetPluginError(char* buffer, unsigned size,
                           const std::string& message) {
    if (!buffer || size == 0) return;
    const size_t count = message.size() < size - 1 ? message.size() : size - 1;
    std::memcpy(buffer, message.data(), count);
    buffer[count] = '\0';
}

// 把插件写入 UTF-16 缓冲（用于宿主内部错误信息）。
inline std::string Utf8Or(const char* value, const char* fallback = "") {
    return (value && *value) ? std::string(value) : std::string(fallback);
}

}  // namespace ttplayer::plugins
