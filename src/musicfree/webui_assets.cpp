// 音乐窗 WebView2 页面资产 ——「外部覆盖优先 + 嵌资源兜底」实现
// 职责与契约见 webui_assets.h 文件头注释。
//
// 设计要点：
//   · 读嵌资源：FindResourceW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(id),
//     RT_RCDATA) + LoadResource；拿不到资源 → 走失败分支返回空串（宿主回退）；
//   · 解包目录候选：%LOCALAPPDATA%\TTPlayer\webui 优先，
//     %TEMP%\TTPlayer\webui 兜底；两条都失败返回空串；
//   · 全路径 try/catch 兜底：任何异常只落日志并返回空串，绝不向宿主抛出；
//   · 日志与 protocol_web.cpp 的 AppendProtoFailLog 同款路径算法
//     （GetModuleFileNameW(nullptr) → exe 目录 → video\bridge.log），
//     同样内容只写一次（失败 / 模式切换才追加），不刷屏。

#include "webui_assets.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

namespace ttplayer::musicfree {
namespace {

// 进程内状态：当前模式、根目录与上一条日志（用于内容去重）。
// PrepareWebuiRoot / WebuiMode 持锁访问；日志在锁内写，避免并发交错。
struct WebuiState {
    std::mutex mutex;
    const char* mode = "";  // "external" / "embedded" / ""
    std::wstring root;      // 当前模式对应的页面根目录
    std::string last_log;   // 上次落盘的日志行，相同则跳过
};

WebuiState& State() {
    static WebuiState state;
    return state;
}

// exe 所在目录（带尾反斜杠）；定位失败返回空串。
std::wstring ExeDir() {
    wchar_t buf[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::wstring path(buf, n);
    const size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return {};
    return path.substr(0, pos + 1);
}

// 读一条 RT_RCDATA 资源为字节块；任何一步失败返回 false。
bool LoadRcResource(int id, std::string& out) {
    HMODULE module = GetModuleHandleW(nullptr);
    if (!module) return false;
    HRSRC res = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!res) return false;
    HGLOBAL mem = LoadResource(module, res);
    if (!mem) return false;
    const void* data = LockResource(mem);
    const DWORD size = SizeofResource(module, res);
    if (!data || size == 0) return false;
    out.assign(static_cast<const char*>(data), static_cast<size_t>(size));
    return true;
}

// 路径是否为存在的普通文件。
bool IsExistingFile(const std::wstring& path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES &&
           (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// 逐级创建目录（含中间层）；已存在且是目录返回 true，失败返回 false。
bool EnsureDirectory(const std::wstring& dir) {
    if (dir.empty()) return false;
    const DWORD attr = GetFileAttributesW(dir.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES)
        return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const size_t pos = dir.find_last_of(L"\\/");
    // 盘符根（如 "C:"，pos==2）不再上溯，直接尝试创建；其余先保证父目录存在。
    if (pos != std::wstring::npos && pos > 2) {
        if (!EnsureDirectory(dir.substr(0, pos))) return false;
    }
    return CreateDirectoryW(dir.c_str(), nullptr) != FALSE ||
           GetLastError() == ERROR_ALREADY_EXISTS;
}

// 整块覆盖写文件。
bool WriteFileBytes(const std::wstring& path, const std::string& data) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    if (!data.empty()) {
        const DWORD size = static_cast<DWORD>(data.size());
        DWORD written = 0;
        ok = WriteFile(file, data.data(), size, &written, nullptr) != FALSE &&
             written == size;
    }
    CloseHandle(file);
    return ok;
}

// 读整个小文件（>4MB 视为异常，防 marker 被换成大文件）；失败返回 false。
bool ReadFileBytes(const std::wstring& path, std::string& out) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER large{};
    if (!GetFileSizeEx(file, &large) || large.QuadPart < 0 ||
        large.QuadPart > 4 * 1024 * 1024) {
        CloseHandle(file);
        return false;
    }
    out.resize(static_cast<size_t>(large.QuadPart));
    bool ok = true;
    if (!out.empty()) {
        const DWORD size = static_cast<DWORD>(out.size());
        DWORD read = 0;
        ok = ReadFile(file, out.data(), size, &read, nullptr) != FALSE &&
             read == size;
    }
    CloseHandle(file);
    if (!ok) out.clear();
    return ok;
}

// 宽串 → UTF-8（只用于日志行拼接）。
std::string ToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int needed =
        WideCharToMultiByte(CP_UTF8, 0, text.data(),
                            static_cast<int>(text.size()), nullptr, 0,
                            nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), needed, nullptr, nullptr);
    return out;
}

// <exe>\video\bridge.log —— 与 protocol_web.cpp 的 PlayFailLogPath 同款算法：
// GetModuleFileNameW(nullptr) 取 exe 所在目录再拼路径（落在 video 目录，
// 与 play_fail.log / bridge 桥接日志同处）。
std::wstring BridgeLogPath() {
    const std::wstring exe_dir = ExeDir();
    if (exe_dir.empty()) return {};
    return exe_dir + L"video\\bridge.log";
}

// 写一行日志（调用方需持 State().mutex）：内容与上次相同则跳过，
// 保证「失败 / 模式切换」各只记一次，不刷屏。
void LogOnce(const std::string& line) {
    WebuiState& state = State();
    if (line == state.last_log) return;
    state.last_log = line;

    const std::wstring path = BridgeLogPath();
    if (path.empty()) return;
    // video 目录可能尚未存在（精简部署），先兜底建一下；建不了就放弃写日志。
    const size_t pos = path.find_last_of(L"\\/");
    if (pos != std::wstring::npos) EnsureDirectory(path.substr(0, pos));
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"ab") != 0 || !file) return;
    char stamp[64]{};
    SYSTEMTIME time{};
    GetLocalTime(&time);
    std::snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d %02d:%02d:%02d",
                  time.wYear, time.wMonth, time.wDay, time.wHour,
                  time.wMinute, time.wSecond);
    std::fprintf(file, "\n==== %s ====\n%s\n", stamp, line.c_str());
    std::fclose(file);
}

// 资源内容指纹（FNV-1a 64）。
// ★ 踩坑：旧 marker 只拼三个资源的**字节长度**（"3001=xxx;3002=yyy;..."），
//   改 app.js 但字数不变时 marker 不变 → 已解包目录被判为「当前」，
//   程序继续加载 %LOCALAPPDATA%\TTPlayer\webui 里的旧页面，
//   表现为「改了页面反复不生效、老问题一直复现」。必须按内容算指纹。
std::string ContentHash(const std::string& data) {
    std::uint64_t h = 14695981039346656037ull;
    for (const unsigned char ch : data) {
        h ^= ch;
        h *= 1099511628211ull;
    }
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%016llX",
                  static_cast<unsigned long long>(h));
    return std::string(buf);
}

// 该目录已是有效的嵌资源落地：三个文件都在，且 marker 与当前资源内容一致。
bool UnpackedCurrent(const std::wstring& dir, const std::string& marker) {
    if (!IsExistingFile(dir + L"\\index.html") ||
        !IsExistingFile(dir + L"\\style.css") ||
        !IsExistingFile(dir + L"\\app.js")) {
        return false;
    }
    std::string got;
    if (!ReadFileBytes(dir + L"\\version.marker", got)) return false;
    return got == marker;
}

// 把三件套解包到目录；marker 写失败不算失败（只影响下次重复解包，
// 页面文件已可用）。成功返回 true。
bool UnpackTo(const std::wstring& dir, const std::string& html,
              const std::string& css, const std::string& js,
              const std::string& marker) {
    if (!EnsureDirectory(dir)) return false;
    if (!WriteFileBytes(dir + L"\\index.html", html)) return false;
    if (!WriteFileBytes(dir + L"\\style.css", css)) return false;
    if (!WriteFileBytes(dir + L"\\app.js", js)) return false;
    WriteFileBytes(dir + L"\\version.marker", marker);
    return true;
}

// 实际准备逻辑（调用方持锁）：失败只落日志并返回空串，绝不向宿主抛出。
std::wstring PrepareLocked() {
    const std::wstring exe_dir = ExeDir();
    if (exe_dir.empty()) {
        LogOnce("webui prepare failed: 无法定位 exe 目录");
        return L"";
    }

    // 1) 外部覆盖优先：<exe>\webui\index.html 存在 → 直接用（改页面免重编）。
    const std::wstring external = exe_dir + L"webui";
    if (IsExistingFile(external + L"\\index.html")) {
        State().mode = "external";
        State().root = external;
        LogOnce("webui mode=external root=" + ToUtf8(external));
        return external;
    }

    // 2) 嵌资源解包：三件套缺一不可。
    std::string html, css, js;
    if (!LoadRcResource(IDR_WEBUI_INDEX, html) ||
        !LoadRcResource(IDR_WEBUI_STYLE, css) ||
        !LoadRcResource(IDR_WEBUI_SCRIPT, js) || html.empty() || css.empty() ||
        js.empty()) {
        State().mode = "";
        State().root.clear();
        LogOnce("webui prepare failed: 嵌资源缺失或为空(IDR_WEBUI_INDEX/STYLE/SCRIPT)"
                " 且无外部 " + ToUtf8(external + L"\\index.html"));
        return L"";
    }

    // marker：资源内容指纹组合（v2）。任一字节变了就重新解包；
    // v1 用尺寸，字数不变的改动会被漏掉（见 ContentHash 注释）。
    const std::string marker = "v2=" + ContentHash(html) + ";" +
                               ContentHash(css) + ";" + ContentHash(js);

    // 3) 解包目录候选：%LOCALAPPDATA% 优先，%TEMP% 兜底。
    std::wstring candidates[2];
    wchar_t buf[1024]{};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, 1024);
    if (n > 0 && n < 1024)
        candidates[0] = std::wstring(buf) + L"\\TTPlayer\\webui";
    wchar_t temp[MAX_PATH]{};
    const DWORD tn = GetTempPathW(MAX_PATH, temp);
    if (tn > 0 && tn < MAX_PATH)
        candidates[1] = std::wstring(temp) + L"TTPlayer\\webui";

    for (const std::wstring& dir : candidates) {
        if (dir.empty()) continue;
        if (UnpackedCurrent(dir, marker) ||
            UnpackTo(dir, html, css, js, marker)) {
            State().mode = "embedded";
            State().root = dir;
            LogOnce("webui mode=embedded root=" + ToUtf8(dir));
            return dir;
        }
    }

    State().mode = "";
    State().root.clear();
    LogOnce("webui prepare failed: 解包目录均不可写(LOCALAPPDATA + TEMP 均失败)");
    return L"";
}

}  // namespace

std::wstring PrepareWebuiRoot() {
    std::lock_guard<std::mutex> lock(State().mutex);
    try {
        return PrepareLocked();
    } catch (...) {
        // 契约：绝不向宿主抛异常（内存/字符串异常也只落日志返回空串）。
        State().mode = "";
        State().root.clear();
        try {
            LogOnce("webui prepare failed: 未捕获异常");
        } catch (...) {
        }
        return L"";
    }
}

const char* WebuiMode() {
    std::lock_guard<std::mutex> lock(State().mutex);
    return State().mode;  // 指向字符串字面量，解锁后仍有效
}

}  // namespace ttplayer::musicfree
