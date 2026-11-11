#include "ttplayer/video/spider/spider_http.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#include <windows.h>

#include "ttplayer/core/text.h"
#include "ttplayer/video/video_cms.h"

namespace ttplayer::video {

namespace {

std::mutex g_log_mutex;

std::string ToLower(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value)
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

std::string NowStamp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char text[64]{};
    std::snprintf(text, sizeof(text), "%04u-%02u-%02u %02u:%02u:%02u.%03u",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                  st.wMilliseconds);
    return text;
}

void AppendLogLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    const std::filesystem::path path = SpiderHttpLogPath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::app | std::ios::binary);
    if (file) file << line << "\r\n";
    const std::wstring wide = core::Utf8ToWide(line);
    ::OutputDebugStringW((wide + L"\r\n").c_str());
}

// 响应摘要素材：压成一行并截断，避免把整页 HTML 灌进日志。
std::string BodySnippet(const std::string& body) {
    std::string snippet = body.substr(0, 512);
    for (char& c : snippet)
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    return snippet;
}

} // namespace

std::string MaskSpiderUrl(std::string_view url) {
    std::string text(url);
    const std::size_t query = text.find('?');
    if (query == std::string::npos) return text;
    const std::size_t hash = text.find('#', query);
    const std::size_t query_end =
        hash == std::string::npos ? text.size() : hash;
    const std::string query_text =
        text.substr(query + 1, query_end - query - 1);

    std::string out = text.substr(0, query + 1);
    bool first = true;
    std::size_t pos = 0;
    while (true) {
        const std::size_t amp = query_text.find('&', pos);
        const std::string part = query_text.substr(
            pos, amp == std::string::npos ? std::string::npos : amp - pos);
        if (!part.empty()) {
            if (!first) out += "&";
            first = false;
            const std::size_t eq = part.find('=');
            out += (eq == std::string::npos ? part : part.substr(0, eq)) + "=***";
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    if (hash != std::string::npos) out += text.substr(hash);
    return out;
}

std::filesystem::path SpiderHttpLogPath() {
    std::filesystem::path base;
    std::vector<wchar_t> buffer(32768, L'\0');
    const DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(),
                                              static_cast<DWORD>(buffer.size()));
    if (len > 0 && len < buffer.size()) {
        base = std::filesystem::path(std::wstring(buffer.data(), len));
    } else {
        std::vector<wchar_t> temp(MAX_PATH, L'\0');
        const DWORD tlen = GetTempPathW(static_cast<DWORD>(temp.size()), temp.data());
        base = tlen > 0 ? std::filesystem::path(std::wstring(temp.data(), tlen))
                        : std::filesystem::temp_directory_path();
    }
    return base / L"TTPlayer" / L"logs" / L"spider.log";
}

SpiderHttp::SpiderHttp(std::string spider_id, musicfree::NetworkConfig net,
                       VideoSite site)
    : spider_id_(std::move(spider_id)), net_(std::move(net)),
      site_(std::move(site)) {}

void SpiderHttp::SetStage(std::string stage) {
    stage_ = stage.empty() ? "general" : std::move(stage);
}

void SpiderHttp::SetDefaultHeaders(const std::map<std::string, std::string>& headers) {
    for (const auto& kv : headers) default_headers_[kv.first] = kv.second;
}

std::map<std::string, std::string> SpiderHttp::MergeHeaders(
    const std::map<std::string, std::string>& headers) const {
    std::map<std::string, std::string> out;
    auto put = [&out](std::string_view name, const std::string& value) {
        const std::string key = ToLower(name);
        if (key.empty() || value.empty()) return;
        // 头名统一小写：WinHTTP 对同名不同大小写会重复下发并返回 183。
        out[key] = value;
    };
    // 优先级：本次调用 > recipe/默认 > 站点 UA/header > NetworkConfig UA。
    for (const auto& kv : BuildSiteHeaders(site_, net_.user_agent))
        put(kv.first, kv.second);
    for (const auto& kv : default_headers_) put(kv.first, kv.second);
    for (const auto& kv : headers) put(kv.first, kv.second);
    if (out.find("accept") == out.end())
        put("accept", "application/json, text/plain, */*");
    return out;
}

SpiderHttpResponse SpiderHttp::Exchange(
    const std::string& url, const std::string& method,
    const std::map<std::string, std::string>& headers,
    const std::vector<std::uint8_t>& body) {
    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();

    SpiderHttpResponse response;
    std::string failure;
    try {
        const musicfree::HttpExchange exchange = musicfree::PerformHttpRequest(
            url, method, MergeHeaders(headers), body, net_);
        response.status = exchange.status;
        response.body.assign(exchange.body.begin(), exchange.body.end());
        response.headers = exchange.headers;
        // UTF-8 BOM 会让后续 JSON 解析失败。
        if (response.body.size() >= 3 &&
            static_cast<unsigned char>(response.body[0]) == 0xEF &&
            static_cast<unsigned char>(response.body[1]) == 0xBB &&
            static_cast<unsigned char>(response.body[2]) == 0xBF) {
            response.body.erase(0, 3);
        }
        if (response.status == 0) failure = "网络错误（无响应）";
    } catch (const std::exception& e) {
        failure = e.what();
    } catch (...) {
        failure = "未知网络错误";
    }

    const long long elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - started).count();
    Log(method, url, response, elapsed_ms, failure);
    if (!failure.empty()) throw std::runtime_error(failure);
    return response;
}

SpiderHttpResponse SpiderHttp::Get(
    const std::string& url, const std::map<std::string, std::string>& headers) {
    return Exchange(url, "GET", headers, {});
}

SpiderHttpResponse SpiderHttp::Post(
    const std::string& url, std::string_view body,
    const std::map<std::string, std::string>& headers,
    std::string_view content_type) {
    std::map<std::string, std::string> merged = headers;
    bool has_content_type = false;
    for (const auto& kv : merged)
        if (ToLower(kv.first) == "content-type") has_content_type = true;
    if (!has_content_type && !content_type.empty())
        merged["content-type"] = std::string(content_type);
    const std::vector<std::uint8_t> payload(body.begin(), body.end());
    return Exchange(url, "POST", merged, payload);
}

void SpiderHttp::Log(const std::string& method, const std::string& url,
                     const SpiderHttpResponse& response, long long elapsed_ms,
                     const std::string& failure) const {
    std::ostringstream line;
    line << NowStamp() << " [" << (spider_id_.empty() ? "?" : spider_id_)
         << "] stage=" << stage_ << " " << method << " " << MaskSpiderUrl(url)
         << " status=" << response.status << " " << elapsed_ms << "ms"
         << " bytes=" << response.body.size();
    const bool failed = !failure.empty() || response.status == 0 ||
                        response.status >= 400;
    if (failed) {
        line << " FAIL";
        if (!failure.empty()) line << " error=" << failure;
        if (!response.body.empty()) line << " body=" << BodySnippet(response.body);
    }
    AppendLogLine(line.str());
}

} // namespace ttplayer::video
