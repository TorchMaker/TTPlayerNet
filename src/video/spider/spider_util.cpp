#include "ttplayer/video/spider/spider_util.h"

#include <windows.h>
#include <wincrypt.h>
#include <bcrypt.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <random>
#include <stdexcept>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace ttplayer::video::spider_util {

namespace {

constexpr char kHex[] = "0123456789abcdef";

std::string HashHex(ALG_ID alg, std::string_view data) {
    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    if (!CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_AES,
                              CRYPT_VERIFYCONTEXT))
        return {};
    if (!CryptCreateHash(prov, alg, 0, 0, &hash)) {
        CryptReleaseContext(prov, 0);
        return {};
    }
    std::string out;
    if (!data.empty() &&
        !CryptHashData(hash, reinterpret_cast<const BYTE*>(data.data()),
                       static_cast<DWORD>(data.size()), 0)) {
        CryptDestroyHash(hash);
        CryptReleaseContext(prov, 0);
        return {};
    }
    BYTE buf[64]{};
    DWORD len = sizeof(buf);
    if (CryptGetHashParam(hash, HP_HASHVAL, buf, &len, 0))
        out = HexEncode(std::string_view(reinterpret_cast<const char*>(buf), len));
    CryptDestroyHash(hash);
    CryptReleaseContext(prov, 0);
    return out;
}

bool IsUnreserved(unsigned char c) {
    return std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
}

int Base64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

} // namespace

std::string Trim(std::string_view s) {
    std::size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string_view::npos) return {};
    std::size_t e = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(b, e - b + 1));
}

std::string ToLower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

bool StartsWith(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool EndsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string ReplaceAll(std::string s, std::string_view from, std::string_view to) {
    if (from.empty()) return s;
    std::size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

std::vector<std::string> Split(std::string_view s, char sep) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= s.size()) {
        std::size_t end = s.find(sep, start);
        if (end == std::string_view::npos) end = s.size();
        if (end > start) out.emplace_back(s.substr(start, end - start));
        if (end == s.size()) break;
        start = end + 1;
    }
    return out;
}

std::string DigitsOnly(std::string_view s) {
    std::string out;
    for (char c : s)
        if (c >= '0' && c <= '9') out.push_back(c);
    return out;
}

// Java URLEncoder 语义：空格 -> '+'，保留 A-Za-z0-9 与 . - * _，
// 其余按 UTF-8 字节 %XX（TVBox spider 原实现就是 Java，签名依赖此行为）。
std::string UrlEncode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '*') {
            out.push_back(static_cast<char>(c));
        } else if (c == ' ') {
            out.push_back('+');
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0f]);
        }
    }
    return out;
}

std::string UrlDecode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '+') {
            out.push_back(' ');
        } else if (c == '%' && i + 2 < s.size()) {
            auto hex = [](char h) -> int {
                if (h >= '0' && h <= '9') return h - '0';
                if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                return -1;
            };
            int hi = hex(s[i + 1]);
            int lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
            } else {
                out.push_back(c);
            }
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string JoinUrl(std::string_view base, std::string_view path) {
    std::string p = Trim(path);
    if (p.empty()) return std::string(base);
    if (StartsWith(ToLower(p), "http://") || StartsWith(ToLower(p), "https://"))
        return p;
    std::string b = Trim(base);
    if (p.rfind("//", 0) == 0) {
        std::size_t colon = b.find(':');
        return colon == std::string::npos ? p : b.substr(0, colon + 1) + p;
    }
    while (!b.empty() && b.back() == '/') b.pop_back();
    if (p[0] == '/') {
        std::size_t scheme = b.find("://");
        if (scheme == std::string::npos) return b + p;
        std::size_t host_end = b.find('/', scheme + 3);
        return host_end == std::string::npos ? b + p : b.substr(0, host_end) + p;
    }
    std::size_t slash = b.find_last_of('/');
    std::size_t scheme = b.find("://");
    if (slash == std::string::npos || (scheme != std::string::npos && slash < scheme + 3))
        return b + "/" + p;
    return b.substr(0, slash + 1) + p;
}

std::string JoinStrings(const std::vector<std::string>& parts, std::string_view sep) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

std::string QueryParam(std::string_view url, std::string_view key) {
    std::size_t q = url.find('?');
    if (q == std::string_view::npos) return {};
    std::size_t hash = url.find('#', q);
    std::string_view query = url.substr(q + 1, hash == std::string_view::npos
                                                  ? std::string_view::npos
                                                  : hash - q - 1);
    for (const auto& part : Split(query, '&')) {
        std::size_t eq = part.find('=');
        std::string name = eq == std::string::npos ? part : part.substr(0, eq);
        if (name == key)
            return eq == std::string::npos ? std::string() : UrlDecode(part.substr(eq + 1));
    }
    return {};
}

std::string HtmlDecode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out.push_back(s[i]);
            continue;
        }
        std::size_t semi = s.find(';', i);
        if (semi == std::string_view::npos || semi - i > 10) {
            out.push_back('&');
            continue;
        }
        std::string name(s.substr(i + 1, semi - i - 1));
        std::string lower = ToLower(name);
        if (lower == "amp") out.push_back('&');
        else if (lower == "lt") out.push_back('<');
        else if (lower == "gt") out.push_back('>');
        else if (lower == "quot") out.push_back('"');
        else if (lower == "apos") out.push_back('\'');
        else if (lower == "nbsp") out.push_back(' ');
        else if (lower.size() > 1 && lower[0] == '#') {
            try {
                int code = lower[1] == 'x' ? std::stoi(lower.substr(2), nullptr, 16)
                                           : std::stoi(lower.substr(1));
                if (code > 0 && code < 0x80) {
                    out.push_back(static_cast<char>(code));
                } else if (code >= 0x80 && code < 0x800) {
                    out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                } else if (code >= 0x800 && code < 0x10000) {
                    out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                    out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                } else {
                    out.push_back(' ');
                }
            } catch (...) {
                out.append(s.substr(i, semi - i + 1));
            }
        } else {
            out.append(s.substr(i, semi - i + 1));
        }
        i = semi;
    }
    return out;
}

std::string StripTags(std::string_view html) {
    std::string out;
    out.reserve(html.size());
    bool in_tag = false;
    for (char c : html) {
        if (c == '<') in_tag = true;
        else if (c == '>' && in_tag) in_tag = false;
        else if (!in_tag) out.push_back(c);
    }
    return Trim(HtmlDecode(out));
}

std::string HexEncode(std::string_view bytes) {
    std::string out;
    out.reserve(bytes.size() * 2);
    for (unsigned char c : bytes) {
        out.push_back(kHex[c >> 4]);
        out.push_back(kHex[c & 0x0f]);
    }
    return out;
}

bool HexDecode(std::string_view hex, std::string* out) {
    if (!out) return false;
    if (hex.size() % 2) return false;
    out->clear();
    out->reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        auto val = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int hi = val(hex[i]);
        int lo = val(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out->push_back(static_cast<char>((hi << 4) | lo));
    }
    return true;
}

std::string Base64Encode(std::string_view bytes) {
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    while (i + 2 < bytes.size()) {
        unsigned v = (static_cast<unsigned char>(bytes[i]) << 16) |
                     (static_cast<unsigned char>(bytes[i + 1]) << 8) |
                     static_cast<unsigned char>(bytes[i + 2]);
        out.push_back(kTable[(v >> 18) & 0x3F]);
        out.push_back(kTable[(v >> 12) & 0x3F]);
        out.push_back(kTable[(v >> 6) & 0x3F]);
        out.push_back(kTable[v & 0x3F]);
        i += 3;
    }
    if (i < bytes.size()) {
        unsigned v = static_cast<unsigned char>(bytes[i]) << 16;
        bool two = i + 1 < bytes.size();
        if (two) v |= static_cast<unsigned char>(bytes[i + 1]) << 8;
        out.push_back(kTable[(v >> 18) & 0x3F]);
        out.push_back(kTable[(v >> 12) & 0x3F]);
        out.push_back(two ? kTable[(v >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
    return out;
}

bool Base64Decode(std::string_view text, std::string* out) {
    if (!out) return false;
    out->clear();
    int acc = 0;
    int bits = 0;
    for (char c : text) {
        if (c == '=' ) break;
        if (std::isspace(static_cast<unsigned char>(c))) continue;
        int v = Base64Value(c);
        if (v < 0) return false;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out->push_back(static_cast<char>((acc >> bits) & 0xFF));
        }
    }
    return true;
}

std::string Md5Hex(std::string_view data) { return HashHex(CALG_MD5, data); }
std::string Sha1Hex(std::string_view data) { return HashHex(CALG_SHA1, data); }
std::string Sha256Hex(std::string_view data) { return HashHex(CALG_SHA_256, data); }

std::string HmacSha256Hex(std::string_view key, std::string_view data) {
    // 不依赖 CALG_HMAC（部分 Crypto provider 不支持）。按 RFC 2104 手工拼：
    //   K' = SHA256(K)（K>64B）否则补零到 64B
    //   HMAC = SHA256((K'^opad) || SHA256((K'^ipad) || data))
    std::string k(key);
    if (k.size() > 64) {
        const std::string digest = Sha256Hex(k);
        std::string bytes;
        if (!HexDecode(digest, &bytes)) return {};
        k = std::move(bytes);
    }
    k.resize(64, '\0');
    std::string ipad = k;
    std::string opad = k;
    for (size_t i = 0; i < 64; ++i) {
        ipad[i] = static_cast<char>(ipad[i] ^ 0x36);
        opad[i] = static_cast<char>(opad[i] ^ 0x5c);
    }
    std::string inner_input = ipad;
    inner_input.append(data);
    const std::string inner_hex = Sha256Hex(inner_input);
    std::string inner;
    if (!HexDecode(inner_hex, &inner)) return {};
    std::string outer_input = opad;
    outer_input.append(inner);
    return Sha256Hex(outer_input);
}

std::string AesCbcDecrypt(std::string_view key, std::string_view iv,
                          std::string_view ciphertext) {
    if (key.empty() || iv.empty() || ciphertext.empty()) return {};
    const std::size_t klen = key.size();
    if (klen != 16 && klen != 24 && klen != 32) return {};  // AES 仅支持 128/192/256
    if (iv.size() != 16) return {};

    BCRYPT_ALG_HANDLE alg = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
            &alg, BCRYPT_AES_ALGORITHM, nullptr, 0)))
        return {};

    // 同一 alg 句柄可复用，但链式模式需对每个 key 单独设置。
    std::string out;
    BCRYPT_KEY_HANDLE hkey = nullptr;
    NTSTATUS status = BCryptSetProperty(
        alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CBC,
        (ULONG)(wcslen(BCRYPT_CHAIN_MODE_CBC) * sizeof(wchar_t)), 0);
    if (BCRYPT_SUCCESS(status)) {
        status = BCryptGenerateSymmetricKey(
            alg, &hkey, nullptr, 0,
            const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(key.data())),
            static_cast<ULONG>(key.size()), 0);
    }

    if (BCRYPT_SUCCESS(status)) {
        ULONG cb = 0;
        // 第一次调用拿到所需缓冲区大小。
        status = BCryptDecrypt(
            hkey,
            const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(ciphertext.data())),
            static_cast<ULONG>(ciphertext.size()), nullptr,
            const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(iv.data())),
            static_cast<ULONG>(iv.size()), nullptr, 0, &cb, 0);
        if (BCRYPT_SUCCESS(status) && cb > 0) {
            out.resize(cb);
            ULONG got = 0;
            status = BCryptDecrypt(
                hkey,
                const_cast<PUCHAR>(
                    reinterpret_cast<const UCHAR*>(ciphertext.data())),
                static_cast<ULONG>(ciphertext.size()), nullptr,
                const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(iv.data())),
                static_cast<ULONG>(iv.size()),
                reinterpret_cast<PUCHAR>(const_cast<char*>(out.data())),
                static_cast<ULONG>(out.size()), &got, 0);
            if (BCRYPT_SUCCESS(status)) out.resize(got);
        }
    }

    if (hkey) BCryptDestroyKey(hkey);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);

    // PKCS#7 去填充。
    if (!out.empty()) {
        const unsigned char pad = static_cast<unsigned char>(out.back());
        if (pad >= 1 && pad <= 16 &&
            pad <= out.size()) {
            bool ok = true;
            for (std::size_t i = out.size() - pad; i < out.size(); ++i)
                if (static_cast<unsigned char>(out[i]) != pad) { ok = false; break; }
            if (ok) out.resize(out.size() - pad);
        }
    }
    return out;
}

long long UnixSeconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

long long UnixMilliseconds() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string RandomHex(int bytes) {
    if (bytes <= 0) return {};
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, 255);
    std::string raw;
    raw.reserve(bytes);
    for (int i = 0; i < bytes; ++i)
        raw.push_back(static_cast<char>(dist(gen)));
    return HexEncode(raw);
}

std::string UuidV4() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, 255);
    unsigned char b[16];
    for (auto& v : b) v = static_cast<unsigned char>(dist(gen));
    b[6] = static_cast<unsigned char>((b[6] & 0x0F) | 0x40);
    b[8] = static_cast<unsigned char>((b[8] & 0x3F) | 0x80);
    char buf[37];
    std::snprintf(buf, sizeof(buf),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9],
                  b[10], b[11], b[12], b[13], b[14], b[15]);
    return buf;
}

nlohmann::json ParseJsonLoose(std::string_view text) {
    std::size_t b = text.find_first_not_of(" \t\r\n");
    if (b == std::string_view::npos) return nullptr;
    text.remove_prefix(b);
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
        text.remove_prefix(3);

    const std::size_t obj = text.find('{');
    const std::size_t arr = text.find('[');
    std::size_t start = std::min(obj, arr);
    if (start == std::string_view::npos) return nullptr;
    const char close = text[start] == '{' ? '}' : ']';
    const std::size_t end = text.rfind(close);
    if (end == std::string_view::npos || end <= start) return nullptr;
    try {
        return nlohmann::json::parse(text.substr(start, end - start + 1));
    } catch (...) {
        return nullptr;
    }
}

namespace {

struct PathToken {
    bool index = false;
    bool wildcard = false;
    int index_value = 0;
    std::string key;
};

std::vector<PathToken> ParsePath(std::string_view path) {
    std::vector<PathToken> tokens;
    std::size_t i = 0;
    if (!path.empty() && path[0] == '$') ++i;
    while (i < path.size()) {
        if (path[i] == '.') {
            ++i;
            std::size_t s = i;
            while (i < path.size() && path[i] != '.' && path[i] != '[') ++i;
            if (i > s) {
                PathToken t;
                t.key = std::string(path.substr(s, i - s));
                tokens.push_back(std::move(t));
            }
        } else if (path[i] == '[') {
            std::size_t e = path.find(']', i);
            if (e == std::string_view::npos) break;
            std::string inner(path.substr(i + 1, e - i - 1));
            PathToken t;
            t.index = true;
            if (inner == "*") {
                t.wildcard = true;
            } else {
                try {
                    t.index_value = std::stoi(inner);
                } catch (...) {
                    t.index_value = -1;
                }
            }
            tokens.push_back(std::move(t));
            i = e + 1;
        } else {
            std::size_t s = i;
            while (i < path.size() && path[i] != '.' && path[i] != '[') ++i;
            PathToken t;
            t.key = std::string(path.substr(s, i - s));
            tokens.push_back(std::move(t));
        }
    }
    return tokens;
}

} // namespace

std::vector<const nlohmann::json*> JsonFindAll(const nlohmann::json& root,
                                               std::string_view path) {
    std::vector<const nlohmann::json*> cur{&root};
    for (const auto& token : ParsePath(path)) {
        std::vector<const nlohmann::json*> next;
        for (const nlohmann::json* node : cur) {
            if (!node) continue;
            if (!token.index) {
                if (node->is_object()) {
                    auto it = node->find(token.key);
                    if (it != node->end()) next.push_back(&(*it));
                }
            } else if (token.wildcard) {
                if (node->is_array()) {
                    for (const auto& v : *node) next.push_back(&v);
                } else if (node->is_object()) {
                    for (auto it = node->begin(); it != node->end(); ++it)
                        next.push_back(&(*it));
                }
            } else if (node->is_array() && token.index_value >= 0 &&
                       static_cast<std::size_t>(token.index_value) < node->size()) {
                next.push_back(&(*node)[static_cast<std::size_t>(token.index_value)]);
            }
        }
        cur.swap(next);
        if (cur.empty()) break;
    }
    return cur;
}

const nlohmann::json* JsonFind(const nlohmann::json& root, std::string_view path) {
    auto all = JsonFindAll(root, path);
    return all.empty() ? nullptr : all.front();
}

std::string JsonString(const nlohmann::json& obj, std::string_view key,
                       std::string_view def) {
    if (!obj.is_object()) return std::string(def);
    auto it = obj.find(std::string(key));
    if (it == obj.end() || it->is_null()) return std::string(def);
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_integer()) return std::to_string(it->get<long long>());
    if (it->is_number_unsigned())
        return std::to_string(it->get<unsigned long long>());
    if (it->is_number_float()) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%g", it->get<double>());
        return buf;
    }
    if (it->is_boolean()) return it->get<bool>() ? "1" : "0";
    return std::string(def);
}

std::string JsonPick(const nlohmann::json& obj,
                     std::initializer_list<std::string_view> keys,
                     std::string_view def) {
    for (auto key : keys) {
        std::string v = JsonString(obj, key);
        if (!v.empty()) return v;
    }
    return std::string(def);
}

} // namespace ttplayer::video::spider_util
