#pragma once

// 内容插件公共工具（header-only）：WinHTTP GET、文件读取、UTF 转换。
//
// 所有插件都只链接系统库（winhttp / ws2_32 / ole32），不依赖宿主代码，
// 保证插件可以独立编译、独立替换，也满足 Win7 兼容（无启动导入）。

#include <string>
#include <vector>

#include <windows.h>
#include <winhttp.h>

namespace ttplayer::plugins {

inline std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                         static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(),
                        static_cast<int>(value.size()), out.data(), size,
                        nullptr, nullptr);
    return out;
}

inline std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                         static_cast<int>(value.size()),
                                         nullptr, 0);
    if (size <= 0) return {};
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(),
                        static_cast<int>(value.size()), out.data(), size);
    return out;
}

inline bool StartsWithHttp(const std::wstring& value) {
    return _wcsnicmp(value.c_str(), L"http://", 7) == 0 ||
           _wcsnicmp(value.c_str(), L"https://", 8) == 0;
}

inline bool ReadFileBytes(const std::wstring& path, std::string* out,
                          std::string* error) {
    if (!out) return false;
    out->clear();
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file) {
        if (error) *error = "无法打开文件";
        return false;
    }
    char buffer[64 * 1024];
    size_t got = 0;
    while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0)
        out->append(buffer, got);
    fclose(file);
    return true;
}

// 一次 HTTP(S) GET（自动跟随最多 5 次重定向），返回响应体字节。
inline bool HttpGet(const std::wstring& url, int timeout_ms,
                    std::string* body, std::string* error,
                    const wchar_t* user_agent = nullptr) {
    if (!body) return false;
    body->clear();
    std::wstring current = url;
    for (int redirect = 0; redirect < 6; ++redirect) {
        URL_COMPONENTSW parts{};
        parts.dwStructSize = sizeof(parts);
        parts.dwSchemeLength = static_cast<DWORD>(-1);
        parts.dwHostNameLength = static_cast<DWORD>(-1);
        parts.dwUrlPathLength = static_cast<DWORD>(-1);
        parts.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(current.c_str(),
                             static_cast<DWORD>(current.size()), 0, &parts)) {
            if (error) *error = "地址格式不正确";
            return false;
        }
        const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
        std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
        if (parts.dwExtraInfoLength)
            path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
        if (path.empty()) path = L"/";

        HINTERNET session = WinHttpOpen(
            user_agent ? user_agent
                       : L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                         L"AppleWebKit/537.36 (KHTML, like Gecko) "
                         L"Chrome/124.0.0.0 Safari/537.36",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) {
            if (error) *error = "WinHTTP 初始化失败";
            return false;
        }
        WinHttpSetTimeouts(session, timeout_ms, timeout_ms, timeout_ms,
                           timeout_ms);
        HINTERNET connect =
            WinHttpConnect(session, host.c_str(), parts.nPort, 0);
        HINTERNET request = nullptr;
        if (connect) {
            request = WinHttpOpenRequest(
                connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                WINHTTP_DEFAULT_ACCEPT_TYPES,
                parts.nScheme == INTERNET_SCHEME_HTTPS
                    ? WINHTTP_FLAG_SECURE
                    : 0);
        }
        bool ok = false;
        if (request) {
            if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(request, nullptr)) {
                DWORD status = 0;
                DWORD size = sizeof(status);
                WinHttpQueryHeaders(
                    request,
                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                    WINHTTP_NO_HEADER_INDEX);
                if (status >= 300 && status < 400) {
                    wchar_t location[2048]{};
                    DWORD location_size = sizeof(location);
                    if (WinHttpQueryHeaders(
                            request, WINHTTP_QUERY_LOCATION,
                            WINHTTP_HEADER_NAME_BY_INDEX, location,
                            &location_size, WINHTTP_NO_HEADER_INDEX) &&
                        location[0]) {
                        current = location;
                        if (current.find(L"://") == std::wstring::npos)
                            current = std::wstring(
                                          parts.nScheme ==
                                                  INTERNET_SCHEME_HTTPS
                                              ? L"https://"
                                              : L"http://") +
                                      host + current;
                        ok = true;  // 继续下一轮
                    }
                } else if (status >= 200 && status < 300) {
                    for (;;) {
                        DWORD available = 0;
                        if (!WinHttpQueryDataAvailable(request, &available) ||
                            available == 0)
                            break;
                        std::string chunk(available, '\0');
                        DWORD read = 0;
                        if (!WinHttpReadData(request, chunk.data(), available,
                                             &read) ||
                            read == 0)
                            break;
                        chunk.resize(read);
                        body->append(chunk);
                    }
                    if (error) error->clear();
                    ok = true;
                    redirect = 99;  // 结束循环
                } else {
                    if (error) *error = "HTTP " + std::to_string(status);
                }
            } else if (error) {
                *error = "网络请求失败 err=" +
                         std::to_string(GetLastError());
            }
        } else if (error) {
            *error = "无法建立连接";
        }
        if (request) WinHttpCloseHandle(request);
        if (connect) WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        if (redirect == 99) return true;
        if (!ok) return false;
        if (!StartsWithHttp(current)) {
            if (error) *error = "重定向地址无效";
            return false;
        }
    }
    if (error) *error = "重定向次数过多";
    return false;
}

inline std::string UrlEncode(const std::string& value) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 3);
    for (unsigned char ch : value) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
            ch == '.' || ch == '~') {
            out.push_back(static_cast<char>(ch));
        } else {
            out.push_back('%');
            out.push_back(hex[ch >> 4]);
            out.push_back(hex[ch & 0x0F]);
        }
    }
    return out;
}

}  // namespace ttplayer::plugins
