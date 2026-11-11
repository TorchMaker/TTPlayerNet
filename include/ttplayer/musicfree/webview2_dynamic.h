#pragma once

// WebView2Loader.dll 的运行时解析（XP / Win7 兼容版用）。
//
// 现代版链接 WebView2Loader.dll.lib，EXE 启动导入表里就有 WebView2Loader.dll；
// 旧系统上既没有这个 DLL，也没有可用的 WebView2 Runtime，加载器会在进程启动
// 阶段直接拒绝，自己的任何降级代码都跑不到。兼容版因此不链接导入库，改为
// 在真正需要创建浏览器环境时 LoadLibrary + GetProcAddress；找不到就返回失败，
// 由调用方回退到原生 Win32 界面 / 「播放失败」浮层。
//
// 函数签名照 WebView2.h 的声明（stdcall，x86 上导出名带 @16 修饰，故同时尝试
// 修饰名与朴素名）。这里不直接 include WebView2.h：兼容版不需要 COM 接口定义，
// 只需要把指针透传给 loader。

#include <windows.h>

#include <string>

namespace ttplayer::musicfree {

// 成功返回函数指针，失败返回 nullptr。结果缓存（进程内一次解析）。
inline void* ResolveWebView2Entry(const char* plain_name,
                                  const char* decorated_name) {
    static HMODULE s_loader = nullptr;
    static bool s_tried = false;
    if (!s_tried) {
        s_tried = true;
        // 优先用 exe 同目录的绝对路径（Win7 兼容包随包带这个 DLL），避免
        // 依赖 DLL 搜索顺序、也避免当前工作目录劫持。随包文件不在时再退回
        // 按名字加载（系统里可能已有同名 loader）。
        wchar_t exe[MAX_PATH]{};
        const DWORD n = ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
        if (n && n < MAX_PATH) {
            std::wstring path(exe, n);
            const size_t cut = path.find_last_of(L"\\/");
            if (cut != std::wstring::npos) {
                path = path.substr(0, cut) + L"\\WebView2Loader.dll";
                s_loader = ::LoadLibraryW(path.c_str());
            }
        }
        if (!s_loader) s_loader = ::LoadLibraryW(L"WebView2Loader.dll");
    }
    if (!s_loader) return nullptr;
    void* p = reinterpret_cast<void*>(
        ::GetProcAddress(s_loader, plain_name));
    if (!p && decorated_name && *decorated_name)
        p = reinterpret_cast<void*>(
            ::GetProcAddress(s_loader, decorated_name));
    return p;
}

// CreateCoreWebView2EnvironmentWithOptions 的 ABI（x86 上 WINAPI=stdcall，
// 后两个参数是 COM 接口指针，用 void* 透传即可，避免引入 WebView2.h 依赖）。
using CreateWebView2EnvFn = HRESULT(WINAPI*)(PCWSTR, PCWSTR, void*, void*);

inline HRESULT CreateWebView2EnvironmentDynamic(PCWSTR browser_folder,
                                                PCWSTR user_data_folder,
                                                void* options,
                                                void* handler) {
    static const CreateWebView2EnvFn fn =
        reinterpret_cast<CreateWebView2EnvFn>(ResolveWebView2Entry(
            "CreateCoreWebView2EnvironmentWithOptions",
            "_CreateCoreWebView2EnvironmentWithOptions@16"));
    if (!fn) return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
    return fn(browser_folder, user_data_folder, options, handler);
}

}  // namespace ttplayer::musicfree
