#pragma once

// 最小 ICoreWebView2EnvironmentOptions（header-only，本机 SDK 无 WRL 头）。
//
// 存在的唯一理由：Win7 上只能用 109 Runtime，而 loader 默认把
// TargetCompatibleBrowserVersion 设成 SDK 的 128.0.2739.15 —— Runtime 低于它时
// CreateCoreWebView2EnvironmentWithOptions 直接以「版本不兼容」失败。音乐窗宿主
// 以前给 options 传 nullptr，正好踩这个坑；现在两个宿主都通过本类型显式传
// PlanWebView2Runtime() 算出的兼容版本。

#include <windows.h>
// objbase.h 必须先于 WebView2.h：interface 宏由 combaseapi.h 定义。
#include <objbase.h>
#include <WebView2.h>

#include <atomic>
#include <cstring>
#include <string>
#include <utility>

namespace ttplayer::musicfree {

class WebView2EnvOptions : public ICoreWebView2EnvironmentOptions {
public:
    explicit WebView2EnvOptions(std::wstring compatible_version,
                                std::wstring args = {})
        : compatible_version_(std::move(compatible_version)),
          args_(std::move(args)) {}
    WebView2EnvOptions(const WebView2EnvOptions&) = delete;
    WebView2EnvOptions& operator=(const WebView2EnvOptions&) = delete;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** obj) override {
        if (!obj) return E_POINTER;
        if (riid == IID_IUnknown ||
            riid == __uuidof(ICoreWebView2EnvironmentOptions)) {
            *obj = static_cast<ICoreWebView2EnvironmentOptions*>(this);
            AddRef();
            return S_OK;
        }
        *obj = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --ref_;
        if (n == 0) delete this;
        return n;
    }

    HRESULT STDMETHODCALLTYPE get_AdditionalBrowserArguments(
        LPWSTR* value) override {
        if (!value) return E_POINTER;
        *value = CopyString(args_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_AdditionalBrowserArguments(
        LPCWSTR value) override {
        args_ = value ? value : L"";
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_Language(LPWSTR* value) override {
        if (!value) return E_POINTER;
        *value = CopyString(language_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_Language(LPCWSTR value) override {
        language_ = value ? value : L"";
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_TargetCompatibleBrowserVersion(
        LPWSTR* value) override {
        if (!value) return E_POINTER;
        *value = CopyString(compatible_version_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_TargetCompatibleBrowserVersion(
        LPCWSTR value) override {
        compatible_version_ = value ? value : L"";
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_AllowSingleSignOnUsingOSPrimaryAccount(
        BOOL* allow) override {
        if (!allow) return E_POINTER;
        *allow = sso_ ? TRUE : FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_AllowSingleSignOnUsingOSPrimaryAccount(
        BOOL allow) override {
        sso_ = allow == TRUE;
        return S_OK;
    }

private:
    static LPWSTR CopyString(const std::wstring& s) {
        const size_t bytes = (s.size() + 1) * sizeof(wchar_t);
        auto* p = static_cast<LPWSTR>(CoTaskMemAlloc(bytes));
        if (!p) return nullptr;
        std::memcpy(p, s.c_str(), bytes);
        return p;
    }

    std::atomic<ULONG> ref_{1};
    std::wstring compatible_version_;
    std::wstring args_;
    std::wstring language_;
    bool sso_ = false;
};

}  // namespace ttplayer::musicfree
