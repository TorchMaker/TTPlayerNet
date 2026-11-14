// WebView2 运行库按系统版本选择 + 缺失时自动安装。接口见 webview2_runtime.h。

#include "ttplayer/musicfree/webview2_runtime.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "ttplayer/core/text.h"
#include "ttplayer/musicfree/musicfree_http.h"
#include "ttplayer/musicfree/musicfree_types.h"

namespace ttplayer::musicfree {
namespace {

// 官方 Evergreen Bootstrapper：Win10+ 分发最新版，Win7/8/8.1 分发
// 109.0.1518.78。不硬编码 109 离线包地址（第三方镜像不可靠）。
constexpr char kSetupUrl[] =
    "https://go.microsoft.com/fwlink/p/?LinkId=2124703";

// 本项目 SDK 是 1.0.2739.15（浏览器 128）。109 Runtime 低于这个数字时 loader
// 会以「版本不兼容」拒绝创建，所以旧系统/随包 Runtime 一律降到 WebView2 GA 版本。
constexpr wchar_t kModernCompat[] = L"128.0.2739.15";
constexpr wchar_t kLegacyCompat[] = L"88.0.705.50";

constexpr wchar_t kClientGuidSubkeys[][96] = {
    L"SOFTWARE\\WOW6432Node\\Microsoft\\EdgeUpdate\\Clients\\"
    L"{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}",
    L"SOFTWARE\\Microsoft\\EdgeUpdate\\Clients\\"
    L"{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}",
};

std::atomic<bool> g_install_inflight{false};

std::wstring ExeDirectory() {
    wchar_t buf[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (!n || n >= MAX_PATH) return {};
    std::wstring path(buf, n);
    const size_t cut = path.find_last_of(L"\\/");
    return cut == std::wstring::npos ? std::wstring() : path.substr(0, cut);
}

bool FileExists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

void TraceLine(const std::wstring& text) {
    OutputDebugStringW((L"[TTPlayer WebView2] " + text + L"\n").c_str());
    const std::wstring dir = ExeDirectory() + L"\\video";
    if (dir.empty()) return;
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring file = dir + L"\\webview2_runtime.log";
    HANDLE f = CreateFileW(file.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    if (GetFileSize(f, nullptr) == 0) {
        const char bom[] = {'\xEF', '\xBB', '\xBF'};
        DWORD wrote = 0;
        WriteFile(f, bom, 3, &wrote, nullptr);
    } else {
        SetFilePointer(f, 0, nullptr, FILE_END);
    }
    std::string line = "[TTPlayer WebView2] ";
    const int bytes = WideCharToMultiByte(
        CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0,
        nullptr, nullptr);
    if (bytes > 0) {
        std::vector<char> tmp(static_cast<size_t>(bytes));
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                            static_cast<int>(text.size()), tmp.data(), bytes,
                            nullptr, nullptr);
        line.append(tmp.data(), static_cast<size_t>(bytes));
    }
    line += "\r\n";
    DWORD wrote = 0;
    WriteFile(f, line.data(), static_cast<DWORD>(line.size()), &wrote, nullptr);
    CloseHandle(f);
}

// 读 EdgeUpdate 的 pv（已装 Evergreen Runtime 版本）。空串 = 没读到。
std::wstring ReadEvergreenPv() {
    wchar_t buf[64]{};
    for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
        for (const auto& sub : kClientGuidSubkeys) {
            HKEY key = nullptr;
            if (RegOpenKeyExW(root, sub, 0, KEY_READ, &key) != ERROR_SUCCESS)
                continue;
            DWORD cb = static_cast<DWORD>(sizeof(buf) - sizeof(wchar_t));
            DWORD type = 0;
            const LONG rc = RegQueryValueExW(
                key, L"pv", nullptr, &type, reinterpret_cast<LPBYTE>(buf), &cb);
            RegCloseKey(key);
            if (rc == ERROR_SUCCESS &&
                (type == REG_SZ || type == REG_EXPAND_SZ) && buf[0])
                return buf;
        }
    }
    return {};
}

unsigned MajorOf(const std::wstring& pv) {
    unsigned major = 0;
    if (swscanf(pv.c_str(), L"%u.", &major) == 1) return major;
    return 0;
}

// 随包 Fixed Version Runtime：<exe>\WebView2Runtime\ 或其一层的版本子目录，
// 只要目录里有 msedgewebview2.exe 就可用。用户也可以把解压好的 109 离线包
// 放这里（避免联网安装）；TTPLAYER_WEBVIEW2_RUNTIME 可显式覆盖目录。
std::wstring FindBundledRuntime() {
    wchar_t env[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"TTPLAYER_WEBVIEW2_RUNTIME", env, MAX_PATH) >
        0) {
        std::wstring dir(env);
        if (FileExists(dir + L"\\msedgewebview2.exe")) return dir;
    }
    const std::wstring base = ExeDirectory() + L"\\WebView2Runtime";
    if (FileExists(base + L"\\msedgewebview2.exe")) return base;

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((base + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (!std::wcscmp(fd.cFileName, L".") ||
            !std::wcscmp(fd.cFileName, L".."))
            continue;
        const std::wstring dir = base + L"\\" + fd.cFileName;
        if (FileExists(dir + L"\\msedgewebview2.exe")) {
            FindClose(h);
            return dir;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return {};
}

WebView2OsTier CurrentTier() {
    // 直接读 ntdll!RtlGetVersion（动态解析，不受 EXE manifest 的
    // supportedOS 谎报影响），且不依赖只编进主 exe 的 platform 模块。
    using VersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    const auto query = reinterpret_cast<VersionFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    DWORD major = 0, minor = 0;
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (query && query(&info) == 0) {
        major = info.dwMajorVersion;
        minor = info.dwMinorVersion;
    } else {
        const DWORD v = GetVersion();  // 兜底；ntdll 里读不到时才会走这里
        major = LOBYTE(LOWORD(v));
        minor = HIBYTE(LOWORD(v));
    }
    if (major >= 10) return WebView2OsTier::kWin10Plus;
    // Win7 = 6.1，Win8 = 6.2，Win8.1 = 6.3；读不到版本时保守按旧系统
    // 处理（低兼容版本 + 仍允许 Evergreen）。
    if (major == 0 || (major == 6 && minor >= 1))
        return WebView2OsTier::kLegacy;
    return WebView2OsTier::kUnsupported;  // XP / 2003 的 5.x
}

bool WriteSetupExe(const std::wstring& path,
                   const std::vector<std::uint8_t>& bytes) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    BOOL ok = WriteFile(f, bytes.data(), static_cast<DWORD>(bytes.size()),
                        &wrote, nullptr);
    CloseHandle(f);
    return ok && wrote == bytes.size();
}

// 静默安装；返回 true 仅表示安装进程正常退出且退出码为 0。
bool RunSetup(const std::wstring& path) {
    std::wstring cmd = L"\"" + path + L"\" /silent /install";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(path.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, 10 * 60 * 1000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return code == 0;
}

}  // namespace

WebView2RuntimePlan PlanWebView2Runtime() {
    WebView2RuntimePlan plan;
    plan.tier = CurrentTier();
    {
        const wchar_t* tier_name = plan.tier == WebView2OsTier::kWin10Plus
                                       ? L"Win10Plus"
                                       : plan.tier == WebView2OsTier::kLegacy
                                             ? L"Legacy(6.1-6.3)"
                                             : L"Unsupported(5.x)";
        TraceLine(std::wstring(L"PlanWebView2Runtime tier=") + tier_name);
    }
    if (plan.tier == WebView2OsTier::kUnsupported) {
        plan.detail = L"XP 或更早系统不支持 WebView2，回退原生界面";
        TraceLine(L"PlanWebView2Runtime -> 不可用（系统不支持）");
        return plan;
    }

    const std::wstring pv = ReadEvergreenPv();
    const unsigned major = MajorOf(pv);
    const std::wstring bundled = FindBundledRuntime();
    TraceLine(L"PlanWebView2Runtime pv=\"" + pv + L"\" bundled=\"" + bundled +
              L"\"");

    // 防 shim 谎报：系统被兼容性 shim（如 WIN7RTM，注册表
    // AppCompatFlags\Layers，常由程序兼容性助手或安装包自动挂上）改写后，
    // RtlGetVersion 会返回 6.x，导致 Win10/11 被误判成 Win7。Evergreen
    // 110+ 的安装器在真实 Win7/8 上无法安装——已装 110+ 就证明系统不是
    // 旧系统，把 tier 纠正回 Win10Plus。
    if (plan.tier == WebView2OsTier::kLegacy && major > 109) {
        plan.tier = WebView2OsTier::kWin10Plus;
        TraceLine(L"PlanWebView2Runtime 检测到 shim 谎报（pv>109 但系统报旧），"
                  L"纠正为 Win10Plus");
    }

    // 随包 Fixed Version 优先级最高：不依赖安装、不需要管理员权限，Win7 上
    // 只要放好 109 目录就能用（离线机器也行）。
    if (!bundled.empty()) {
        plan.browser_folder = bundled;
        plan.compatible_version = kLegacyCompat;
        plan.available = true;
        plan.detail = L"随包 Fixed Version Runtime：" + bundled;
        return plan;
    }

    if (plan.tier == WebView2OsTier::kWin10Plus) {
        if (!pv.empty() && pv != L"0.0.0.0") {
            plan.browser_folder.clear();
            // Win10 上也可能残留 109（例如从 Win7 升级），此时不能用 128。
            plan.compatible_version =
                major >= 128 ? kModernCompat : kLegacyCompat;
            plan.available = true;
            plan.detail = L"系统 Evergreen Runtime " + pv;
            TraceLine(L"PlanWebView2Runtime -> 可用：" + plan.detail);
            return plan;
        }
        plan.detail = L"Win10+ 未安装 WebView2 Runtime，需自动安装";
        TraceLine(L"PlanWebView2Runtime -> 不可用：pv 为空或 0.0.0.0");
        return plan;
    }

    // Win7 / 8 / 8.1：只有 109 及以下能跑。
    if (major > 0 && major <= 109) {
        plan.browser_folder.clear();
        plan.compatible_version = kLegacyCompat;
        plan.available = true;
        plan.detail = L"已装旧系统兼容 Evergreen Runtime " + pv;
        TraceLine(L"PlanWebView2Runtime -> 可用（旧系统 109）：pv=" + pv);
    } else if (major > 109) {
        plan.detail = L"已装 Runtime " + pv + L" 高于 109，旧系统无法运行";
        TraceLine(L"PlanWebView2Runtime -> 不可用：系统被判为旧系统且 pv>109");
    } else {
        plan.detail = L"未安装 WebView2 Runtime，需自动安装 109";
        TraceLine(L"PlanWebView2Runtime -> 不可用：旧系统且 pv 为空");
    }
    return plan;
}

bool EnsureWebView2RuntimeAsync(HWND notify_hwnd, UINT notify_msg) {
    if (CurrentTier() == WebView2OsTier::kUnsupported) return false;
    if (GetEnvironmentVariableW(L"TTPLAYER_NO_WEBVIEW_INSTALL", nullptr, 0) > 0)
        return false;
    bool expected = false;
    if (!g_install_inflight.compare_exchange_strong(expected, true))
        return false;

    std::thread([notify_hwnd, notify_msg] {
        struct Guard {
            ~Guard() { g_install_inflight.store(false); }
        } guard;
        bool installed = false;
        try {
            TraceLine(L"开始下载 WebView2 Evergreen Bootstrapper");
            NetworkConfig cfg;
            const auto bytes = DownloadBinary(kSetupUrl, cfg, {});
            if (bytes.size() < 64 * 1024 || bytes[0] != 'M' || bytes[1] != 'Z')
                throw std::runtime_error("bootstrapper 内容不合法");

            wchar_t temp[MAX_PATH]{};
            const DWORD n = GetTempPathW(MAX_PATH, temp);
            if (!n || n >= MAX_PATH) throw std::runtime_error("取临时目录失败");
            const std::wstring path =
                std::wstring(temp, n) + L"TTPlayer-WebView2Setup.exe";
            if (!WriteSetupExe(path, bytes))
                throw std::runtime_error("写安装包失败");

            TraceLine(L"Bootstrapper 已下载，开始静默安装（Win7 会装 109）");
            installed = RunSetup(path);
            TraceLine(installed ? L"安装完成"
                                : L"安装程序返回非零，保留原生界面回退");
            DeleteFileW(path.c_str());
        } catch (const std::exception& e) {
            TraceLine(std::wstring(L"自动安装失败：") +
                      core::Utf8ToWide(e.what()));
        } catch (...) {
            TraceLine(L"自动安装失败：未知异常");
        }

        // 重新检测：确实装上了才通知音乐窗重试创建（避免无谓重建宿主）。
        if (installed && PlanWebView2Runtime().available) {
            if (notify_hwnd && IsWindow(notify_hwnd))
                PostMessageW(notify_hwnd, notify_msg, 0, 0);
        }
    }).detach();
    return true;
}

bool WebView2RuntimeInstallInFlight() {
    return g_install_inflight.load();
}

}  // namespace ttplayer::musicfree
