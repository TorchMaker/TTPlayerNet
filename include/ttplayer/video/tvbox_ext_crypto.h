#pragma once

// TVBox 站点 ext 字段解密 —— ftyguard / 饭太硬系配置专用，零依赖。
//
// 很多 TVBox 配置把站点的真实接口地址放在 site.ext 里，并做了一层加密：
//
//     ext = base64( RC4(key="fantaiying921", plaintext) 每字节 +2 )
//
// 解密顺序（实测）：
//     base64_decode → RC4(key) → 每字节 -2
//
// 明文常是 {"siteUrl":"http://.../api.php/provide/vod/...","type":1,...}，
// 里面的分类名可能是 GBK 字节，所以这里不要求整体是合法 UTF-8 —— 调用方
// 只扫描 ASCII 的 http(s) URL 即可，不做编码转换。

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ttplayer::video::tvbox_ext {

inline int B64Val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

inline bool Base64Decode(std::string_view s, std::string& out) {
    out.clear();
    int buf = 0, bits = 0;
    for (char c : s) {
        if (c == '=' || c == '\r' || c == '\n' || c == ' ' || c == '\t') break;
        const int v = B64Val(c);
        if (v < 0) return false;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buf >> bits) & 0xFF));
        }
    }
    return !out.empty();
}

inline std::string Rc4(const std::uint8_t* data, std::size_t len,
                       const std::string& key) {
    std::uint8_t S[256];
    for (int i = 0; i < 256; ++i) S[i] = static_cast<std::uint8_t>(i);
    int j = 0;
    for (int i = 0; i < 256; ++i) {
        j = (j + S[i] + static_cast<std::uint8_t>(key[i % key.size()])) & 255;
        const std::uint8_t t = S[i]; S[i] = S[j]; S[j] = t;
    }
    std::string out(len, '\0');
    int a = 0;
    j = 0;
    for (std::size_t n = 0; n < len; ++n) {
        a = (a + 1) & 255;
        j = (j + S[a]) & 255;
        const std::uint8_t t = S[a]; S[a] = S[j]; S[j] = t;
        out[n] = static_cast<char>(data[n] ^ S[(S[a] + S[j]) & 255]);
    }
    return out;
}

// 已知密钥表。不同 jar 可能换 key，新增时只要往这里加一项。
inline const char* const kExtKeys[] = {"fantaiying921"};

inline bool TryDecryptExt(std::string_view text, std::string& out) {
    if (text.size() < 16) return false;
    // 明文 ext（直接是 URL / JSON / 相对路径）不需要解，交给调用方。
    if (text.find("http") != std::string_view::npos ||
        text.front() == '{' || text.rfind("./", 0) == 0)
        return false;

    std::string bin;
    if (!Base64Decode(text, bin)) return false;
    for (const char* key : kExtKeys) {
        std::string plain = Rc4(
            reinterpret_cast<const std::uint8_t*>(bin.data()), bin.size(), key);
        for (char& c : plain)
            c = static_cast<char>(static_cast<std::uint8_t>(c) - 2);
        // 成功判据：看得到 JSON 起始或 http 地址。明文里的中文分类名可能是
        // GBK，不做整体 UTF-8 校验。
        if (plain.find("http") != std::string::npos ||
            plain.find("siteUrl") != std::string::npos ||
            (!plain.empty() && plain[0] == '{')) {
            out = std::move(plain);
            return true;
        }
    }
    return false;
}

}  // namespace ttplayer::video::tvbox_ext
