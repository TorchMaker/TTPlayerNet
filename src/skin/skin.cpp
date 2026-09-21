#include "ttplayer/skin/skin.h"
#include "ttplayer/skin/skin_frame.h"
#include "ttplayer/settings/settings.h"

#include <algorithm>
#include <array>
#include <comdef.h>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <msxml6.h>
#include <regex>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace ttplayer::skin {
namespace {
struct ComScope {
    HRESULT result{CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)};
    ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
};

std::wstring Attribute(IXMLDOMNode* node, const wchar_t* name) {
    IXMLDOMNamedNodeMap* attributes{};
    IXMLDOMNode* attribute{};
    if (!node || FAILED(node->get_attributes(&attributes)) || !attributes) return {};
    attributes->getNamedItem(_bstr_t(name), &attribute);
    attributes->Release();
    if (!attribute) return {};
    VARIANT value{};
    VariantInit(&value);
    attribute->get_nodeValue(&value);
    attribute->Release();
    const _variant_t managed(value, false);
    if (managed.vt == VT_EMPTY || managed.vt == VT_NULL) return {};
    return static_cast<const wchar_t*>(_bstr_t(managed));
}

RECT ParseRect(const std::wstring& value) {
    RECT result{};
    if (swscanf_s(value.c_str(), L"%ld , %ld , %ld , %ld",
                  &result.left, &result.top, &result.right, &result.bottom) != 4) return {};
    return result;
}

COLORREF ParseColor(const std::wstring& value, COLORREF fallback) {
    unsigned int red{}, green{}, blue{};
    if (swscanf_s(value.c_str(), L"#%2x%2x%2x",
                  &red, &green, &blue) != 3) return fallback;
    return RGB(red, green, blue);
}

int ParseInt(const std::wstring& value, int fallback) {
    if (value.empty()) return fallback;
    wchar_t* end{};
    const long parsed = wcstol(value.c_str(), &end, 10);
    return end && *end == L'\0' ? static_cast<int>(parsed) : fallback;
}

unsigned int ParseAlignment(const std::wstring& value) {
    unsigned int result{};
    size_t first{};
    while (first <= value.size()) {
        const size_t separator = value.find(L'+', first);
        const std::wstring_view token(value.data() + first,
            (separator == std::wstring::npos ? value.size() : separator) - first);
        if (token == L"left") result = (result & 0xf0U) | 1U;
        else if (token == L"center") result = (result & 0xf0U) | 2U;
        else if (token == L"right") result = (result & 0xf0U) | 3U;
        else if (token == L"top") result = (result & 0x0fU) | 0x10U;
        else if (token == L"vcenter") result = (result & 0x0fU) | 0x20U;
        else if (token == L"bottom") result = (result & 0x0fU) | 0x30U;
        if (separator == std::wstring::npos) break;
        first = separator + 1;
    }
    return result;
}

std::wstring DecodeLegacyXml(std::span<const unsigned char> source) {
    std::string bytes(reinterpret_cast<const char*>(source.data()), source.size());
    if (bytes.starts_with("\xEF\xBB\xBF")) bytes.erase(0, 3);
    UINT code_page = CP_UTF8;
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                                    static_cast<int>(bytes.size()), nullptr, 0);
    if (count <= 0) {
        code_page = CP_ACP;
        count = MultiByteToWideChar(code_page, 0, bytes.data(),
                                    static_cast<int>(bytes.size()), nullptr, 0);
    }
    if (count <= 0) throw std::runtime_error("unsupported Skin.xml encoding");
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(code_page, code_page == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0,
                        bytes.data(), static_cast<int>(bytes.size()), result.data(), count);
    // TTPlayer's legacy XML reader accepts two defects present in the shipped
    // skin collection: unquoted scalar attributes and adjacent quoted
    // attributes without intervening whitespace.  Normalize both before
    // handing the document to MSXML.  Excluding '/' keeps `value=true/>` as a
    // self-closing tag rather than consuming its terminator into the value.
    result = std::regex_replace(result,
        std::wregex(L"([\"'])([A-Za-z_][A-Za-z0-9_-]*\\s*=)"),
        L"$1 $2");
    // Quote unquoted attribute values only while walking a tag and only when
    // the '=' is outside an existing quoted value.  A regular expression over
    // the whole document also sees query-string pairs inside url="..." and
    // corrupts strings such as `?automodule=blog&blogid=62`.
    std::wstring normalized;
    normalized.reserve(result.size());
    bool in_tag{};
    wchar_t quote{};
    for (size_t index{}; index < result.size();) {
        const wchar_t value = result[index];
        if (!in_tag) {
            normalized.push_back(value);
            if (value == L'<') in_tag = true;
            ++index;
            continue;
        }
        if (quote) {
            normalized.push_back(value);
            if (value == quote) quote = 0;
            ++index;
            continue;
        }
        if (value == L'\"' || value == L'\'') {
            quote = value;
            normalized.push_back(value);
            ++index;
            continue;
        }
        if (value == L'>') {
            in_tag = false;
            normalized.push_back(value);
            ++index;
            continue;
        }
        normalized.push_back(value);
        ++index;
        if (value != L'=') continue;
        while (index < result.size() && iswspace(result[index]))
            normalized.push_back(result[index++]);
        if (index >= result.size() || result[index] == L'\"' ||
            result[index] == L'\'' || result[index] == L'>' ||
            result[index] == L'/') continue;
        size_t end = index;
        while (end < result.size() && !iswspace(result[end]) &&
               result[end] != L'>' && result[end] != L'\"' &&
               result[end] != L'\'') ++end;
        size_t value_end = end;
        if (value_end > index && result[value_end - 1] == L'/') --value_end;
        normalized.push_back(L'\"');
        normalized.append(result, index, value_end - index);
        normalized.push_back(L'\"');
        normalized.append(result, value_end, end - value_end);
        // Some published skins terminate an otherwise unquoted value with a
        // stray quote (`position=-207,12,0,187," resize_rect=...`).  The
        // native reader treats it as the end delimiter; discard it here.
        index = end < result.size() && (result[end] == L'\"' ||
            result[end] == L'\'') ? end + 1 : end;
    }
    result = std::move(normalized);
    // The same permissive parser also keeps the first occurrence of an
    // immediately repeated attribute.  ouptix.skn contains
    // `align="" align=""`; XML DOM rejects the entire document for it.
    const std::wregex adjacent_duplicate(
        L"(\\s+([A-Za-z_][A-Za-z0-9_-]*)\\s*=\\s*(\\\"[^\\\"]*\\\"|'[^']*'))"
        L"\\s+\\2\\s*=\\s*(\\\"[^\\\"]*\\\"|'[^']*')");
    for (;;) {
        const std::wstring deduplicated = std::regex_replace(
            result, adjacent_duplicate, L"$1");
        if (deduplicated == result) break;
        result = deduplicated;
    }
    // Old skin metadata commonly embeds a query string with a literal '&'
    // in url=. The legacy reader accepts it; XML 1.0 requires escaping it.
    for (size_t offset{}; (offset = result.find(L'&', offset)) !=
                          std::wstring::npos;) {
        const bool entity = result.compare(offset, 5, L"&amp;") == 0 ||
            result.compare(offset, 4, L"&lt;") == 0 ||
            result.compare(offset, 4, L"&gt;") == 0 ||
            result.compare(offset, 6, L"&quot;") == 0 ||
            result.compare(offset, 6, L"&apos;") == 0 ||
            (offset + 2 < result.size() && result[offset + 1] == L'#' &&
             result.find(L';', offset + 2) != std::wstring::npos);
        if (entity) {
            ++offset;
        } else {
            result.replace(offset, 1, L"&amp;");
            offset += 5;
        }
    }
    return result;
}

std::wstring DecodeLegacyXml(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open Skin.xml");
    const std::vector<unsigned char> bytes(
        (std::istreambuf_iterator<char>(input)), {});
    return DecodeLegacyXml(bytes);
}

bool IsFourStateButton(std::wstring_view name) {
    // Exact node set dispatched to FUN_004A95B0 by
    // CSkinParser_ParsePlayerWindow (004A8536).  Ghidra rendered the literal
    // at VA 0051C438 as an empty string, but the PE bytes at file offset
    // 0011F3C8 are "browser\0".  It is therefore one of the native buttons;
    // 6.1.2 (00520101) additionally accepts set and the five mode_* buttons.
    constexpr std::wstring_view names[] = {
        L"play", L"pause", L"stop", L"prev", L"next", L"mute", L"open",
        L"lyric", L"equalizer", L"playlist", L"browser", L"exit",
        L"minimize", L"minimode", L"set", L"login", L"mode_single", L"mode_loop",
        L"mode_slider", L"mode_circle", L"mode_random"
    };
    return std::find(std::begin(names), std::end(names), name) != std::end(names);
}
} // namespace

std::optional<SkinMetadata> ParseLegacySkinMetadata(
    std::span<const unsigned char> xml_bytes) {
    try {
        // DecodeLegacyXml deliberately accepts and strips an UTF-8 BOM.  The
        // native token reader did not, but accepting it is required for
        // otherwise valid packages such as 004941_088.skn.
        ComScope com;
        if (FAILED(com.result) && com.result != RPC_E_CHANGED_MODE)
            return std::nullopt;

        IXMLDOMDocument* document{};
        if (FAILED(CoCreateInstance(__uuidof(DOMDocument60), nullptr,
                                    CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&document)))) {
            return std::nullopt;
        }
        document->put_async(VARIANT_FALSE);
        auto xml = DecodeLegacyXml(xml_bytes);
        // FUN_004A7929 consumes one root element and ignores bytes after its
        // closing tag.  Preserve that behavior for published skins carrying
        // an obsolete coordinate line after </skin>.
        if (const size_t end = xml.find(L"</skin>"); end != std::wstring::npos)
            xml.erase(end + std::wstring_view(L"</skin>").size());

        VARIANT_BOOL loaded{};
        document->loadXML(_bstr_t(xml.c_str()), &loaded);
        if (loaded != VARIANT_TRUE) {
            document->Release();
            return std::nullopt;
        }

        IXMLDOMNode* root{};
        document->selectSingleNode(_bstr_t(L"/skin"), &root);
        if (!root) {
            document->Release();
            return std::nullopt;
        }

        SkinMetadata result;
        result.version = ParseInt(Attribute(root, L"version"), 0);
        if (result.version == 2) {
            result.name = Attribute(root, L"name");
            result.author = Attribute(root, L"author");
            result.url = Attribute(root, L"url");
            result.email = Attribute(root, L"email");
            if (result.email.size() >= 7 &&
                _wcsnicmp(result.email.c_str(), L"mailto:", 7) == 0) {
                result.email.erase(0, 7);
            }
            result.transparent_color = ParseColor(
                Attribute(root, L"transparent_color"), CLR_INVALID);
        }
        root->Release();
        document->Release();
        if (result.version != 2) return std::nullopt;
        return result;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

LegacySkin::~LegacySkin() {
    if (icon_) DestroyIcon(icon_);
}

LegacySkin::LegacySkin(LegacySkin&& other) noexcept {
    *this = std::move(other);
}

// 这是手写的成员搬运表（不能靠默认实现：被搬走的 SkinImage 必须显式清空，否则
// moved-from 对象析构时会重复释放 HBITMAP）。代价是**每加一个皮肤段都必须在这里
// 补一行** —— browser_ 漏掉过一次：LoadSkin 内部是 skin_.emplace(std::move(next))，
// 于是音乐窗的 <browser_window> 段每次都被静默丢弃，窗口永远退化成自绘兜底框。
LegacySkin& LegacySkin::operator=(LegacySkin&& other) noexcept {
    if (this == &other) return *this;
    if (icon_) DestroyIcon(icon_);
    bitmaps_ = std::move(other.bitmaps_);
    // Layouts/cache share RAII images. Clear moved-from references as well.
    other.bitmaps_.clear();
    background_ = std::exchange(other.background_, nullptr);
    icon_ = std::exchange(other.icon_, nullptr);
    window_size_ = other.window_size_;
    transparent_color_ = other.transparent_color_;
    elements_ = std::move(other.elements_);
    other.elements_.clear();
    mini_ = std::move(other.mini_);
    other.mini_ = {};
    lyric_ = std::move(other.lyric_);
    other.lyric_ = {};
    desktop_lyric_bar_ = std::move(other.desktop_lyric_bar_);
    other.desktop_lyric_bar_ = {};
    playlist_ = std::move(other.playlist_);
    other.playlist_ = {};
    equalizer_ = std::move(other.equalizer_);
    other.equalizer_ = {};
    browser_ = std::move(other.browser_);
    other.browser_ = {};
    visual_ = std::move(other.visual_);
    other.visual_ = {};
    return *this;
}

SkinImage LegacySkin::LoadBitmap(const std::filesystem::path& path) {
    auto key = path.lexically_normal().wstring();
    std::transform(key.begin(), key.end(), key.begin(), towlower);
    if (const auto found = bitmaps_.find(key); found != bitmaps_.end()) return found->second;
    const auto bitmap = SkinImage::Load(path);
    if (!bitmap) return nullptr;
    bitmaps_.emplace(key, bitmap);
    return bitmap;
}

namespace {
SkinAnimation LoadAnimation(IXMLDOMNode* node, int default_mode) {
    SkinAnimation result;
    result.mode = ParseInt(Attribute(node, L"flash_mode"), default_mode);
    result.frame_count = ParseInt(Attribute(node, L"frame_count"), 10);
    result.frame_interval = ParseInt(Attribute(node, L"frame_interval"), 100);
    // 00419637 disables invalid animation parameters, not the image/control.
    if (result.mode < 0 || result.mode > 3 || result.frame_count <= 0 ||
        result.frame_interval <= 0) result.mode = 0;
    return result;
}

void LoadElementAnimation(LegacySkin& skin, const std::filesystem::path& directory,
                          IXMLDOMNode* node, SkinElement& element) {
    element.animation = LoadAnimation(node, element.four_state ? 1 : 0);
    if (element.four_state && element.animation.mode > 2) element.animation.mode = 0;
    const auto flash = Attribute(node, L"flash_image");
    if (!flash.empty()) {
        element.flash_image = skin.LoadBitmap(directory / flash);
        element.flash_size = element.flash_image.Size();
    }
}

SkinBitmap LoadSkinBitmap(LegacySkin& skin, const std::filesystem::path& directory,
                          IXMLDOMNode* node, const wchar_t* attribute) {
    SkinBitmap result;
    const auto name = Attribute(node, attribute);
    if (name.empty()) return result;
    // Keep bitmap ownership centralized in LegacySkin::bitmaps_.
    result.image = skin.LoadBitmap(directory / name);
    if (result.image) {
        BITMAP info{};
        GetObjectW(result.image, sizeof(info), &info);
        result.size = {info.bmWidth, info.bmHeight};
    }
    return result;
}

SkinElement LoadEqualizerElement(LegacySkin& skin,
                                 const std::filesystem::path& directory,
                                 IXMLDOMNode* node, std::wstring name,
                                 bool button, bool force_vertical = false) {
    SkinElement element;
    element.name = std::move(name);
    element.bounds = ParseRect(Attribute(node, L"position"));
    element.alignment = ParseAlignment(Attribute(node, L"align"));
    const auto vertical = Attribute(node, L"vertical");
    element.vertical = force_vertical || _wcsicmp(vertical.c_str(), L"true") == 0 ||
                       vertical == L"1";
    element.four_state = button;
    const auto load = [&](const wchar_t* attribute, SkinImage& bitmap, SIZE& size) {
        const auto file = Attribute(node, attribute);
        if (file.empty()) return;
        bitmap = skin.LoadBitmap(directory / file);
        if (!bitmap) return;
        BITMAP info{};
        GetObjectW(bitmap, sizeof(info), &info);
        size = {info.bmWidth, info.bmHeight};
    };
    load(L"image", element.image, element.image_size);
    load(L"bar_image", element.bar_image, element.bar_size);
    load(L"fill_image", element.fill_image, element.fill_size);
    load(L"fill_image2", element.fill_image2, element.fill_size2);
    load(L"thumb_image", element.thumb_image, element.thumb_size);
    LoadElementAnimation(skin, directory, node, element);
    if (button && element.image) {
        element.frames = 4;
        // FUN_0042955B applies the XML position after FUN_00452FFF installs
        // the four-frame bitmap. The child therefore retains the complete
        // XML rectangle even when a frame is a few pixels smaller (for
        // example Let's Vista profile: 96x21 client, 94x19 frame).
    }
    return element;
}

SkinElement LoadPlaylistImageElement(LegacySkin& skin,
                                     const std::filesystem::path& directory,
                                     IXMLDOMNode* node, std::wstring name) {
    SkinElement element;
    element.name = std::move(name);
    element.bounds = ParseRect(Attribute(node, L"position"));
    element.alignment = ParseAlignment(Attribute(node, L"align"));
    const auto image = Attribute(node, L"image");
    if (!image.empty()) {
        element.image = skin.LoadBitmap(directory / image);
        if (element.image) {
            BITMAP info{};
            GetObjectW(element.image, sizeof(info), &info);
            element.image_size = {info.bmWidth, info.bmHeight};
            if (_wcsicmp(element.name.c_str(), L"close") == 0 &&
                info.bmWidth >= 4) {
                element.four_state = true;
                element.frames = 4;
                // Playlist XML stores the four-frame strip extent and anchors
                // its right edge.  The live button occupies the final frame's
                // width immediately to the left of that anchor.
                element.bounds.left = element.bounds.right - info.bmWidth / 4;
                element.bounds.bottom = element.bounds.top + info.bmHeight;
            }
        }
    }
    LoadElementAnimation(skin, directory, node, element);
    return element;
}

bool ParseBool(const std::wstring& value, bool fallback = false) {
    if (value.empty()) return fallback;
    return _wcsicmp(value.c_str(), L"true") == 0 || value == L"1";
}

// 音乐窗框体里的按钮。位图是横向多帧条带（close/backward/forward/refresh 都是
// 四帧），XML 矩形留了 1~2 像素的视觉留白，因此按帧的实际尺寸居中到矩形里，
// 而不是把单帧拉伸到整个矩形。
SkinElement LoadBrowserButton(LegacySkin& skin, const std::filesystem::path& directory,
                              IXMLDOMNode* node, std::wstring name,
                              const wchar_t* attribute = L"image") {
    SkinElement element;
    element.name = std::move(name);
    const RECT rect = ParseRect(Attribute(node, L"position"));
    element.bounds = rect;
    element.alignment = ParseAlignment(Attribute(node, L"align"));
    const auto image = Attribute(node, attribute);
    if (!image.empty()) {
        element.image = skin.LoadBitmap(directory / image);
        if (element.image) {
            BITMAP info{};
            GetObjectW(element.image, sizeof(info), &info);
            element.image_size = {info.bmWidth, info.bmHeight};
            if (info.bmWidth >= 4) {
                element.four_state = true;
                element.frames = 4;
                const int frame_width = info.bmWidth / 4;
                element.bounds.left = rect.left +
                    std::max<LONG>(0, (rect.right - rect.left - frame_width) / 2);
                element.bounds.top = rect.top +
                    std::max<LONG>(0, (rect.bottom - rect.top - info.bmHeight) / 2);
                element.bounds.right = element.bounds.left + frame_width;
                element.bounds.bottom = element.bounds.top + info.bmHeight;
            }
        }
    }
    element.color = ParseColor(Attribute(node, L"color"), element.color);
    const auto font = Attribute(node, L"font");
    if (!font.empty()) element.font = font;
    element.font_size = ParseInt(Attribute(node, L"font_size"), element.font_size);
    return element;
}

void InitializeDefaultLogFont(LOGFONTW& font) {
    font = {};
    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, 0, &metrics, 0))
        font = metrics.lfMessageFont;
    else {
        font.lfHeight = -12;
        font.lfWeight = FW_NORMAL;
        wcscpy_s(font.lfFaceName, L"SimSun");
    }
}

std::vector<unsigned char> DecodeBase64(std::wstring_view source) {
    std::array<int, 256> values{};
    values.fill(-1);
    constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int index = 0; index < 64; ++index)
        values[static_cast<unsigned char>(alphabet[index])] = index;
    std::vector<unsigned char> output;
    output.reserve(source.size() * 3 / 4);
    unsigned int accumulator{};
    int bits{};
    for (const wchar_t wide : source) {
        if (wide == L'=') break;
        if (iswspace(wide)) continue;
        if (wide < 0 || wide > 0xff || values[static_cast<unsigned char>(wide)] < 0)
            return {};
        accumulator = (accumulator << 6) |
                      static_cast<unsigned int>(values[static_cast<unsigned char>(wide)]);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            output.push_back(static_cast<unsigned char>(accumulator >> bits));
            accumulator &= (1U << bits) - 1U;
        }
    }
    return output;
}

bool ParseLogFont(const std::wstring& descriptor, LOGFONTW& font) {
    if (descriptor.empty()) return false;
    LOGFONTW parsed = font;
    if (_wcsnicmp(descriptor.c_str(), L"base64:", 7) == 0) {
        const auto bytes = DecodeBase64(
            std::wstring_view(descriptor).substr(7));
        if (bytes.size() == sizeof(LOGFONTW)) {
            std::memcpy(&parsed, bytes.data(), sizeof(parsed));
        } else if (bytes.size() == sizeof(LOGFONTA)) {
            LOGFONTA ansi{};
            std::memcpy(&ansi, bytes.data(), sizeof(ansi));
            parsed = {};
            parsed.lfHeight = ansi.lfHeight;
            parsed.lfWidth = ansi.lfWidth;
            parsed.lfEscapement = ansi.lfEscapement;
            parsed.lfOrientation = ansi.lfOrientation;
            parsed.lfWeight = ansi.lfWeight;
            parsed.lfItalic = ansi.lfItalic;
            parsed.lfUnderline = ansi.lfUnderline;
            parsed.lfStrikeOut = ansi.lfStrikeOut;
            parsed.lfCharSet = ansi.lfCharSet;
            parsed.lfOutPrecision = ansi.lfOutPrecision;
            parsed.lfClipPrecision = ansi.lfClipPrecision;
            parsed.lfQuality = ansi.lfQuality;
            parsed.lfPitchAndFamily = ansi.lfPitchAndFamily;
            if (MultiByteToWideChar(CP_ACP, 0, ansi.lfFaceName, -1,
                    parsed.lfFaceName, LF_FACESIZE) == 0) return false;
        } else {
            return false;
        }
        font = parsed;
        return true;
    }

    std::array<int, 13> value{};
    if (swscanf_s(descriptor.c_str(),
            L"%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,",
            &value[0], &value[1], &value[2], &value[3], &value[4],
            &value[5], &value[6], &value[7], &value[8], &value[9],
            &value[10], &value[11], &value[12]) != 13) return false;
    size_t comma = 0;
    for (size_t index = 0; index < value.size(); ++index) {
        comma = descriptor.find(L',', comma);
        if (comma == std::wstring::npos) return false;
        ++comma;
    }
    parsed.lfHeight = value[0];
    parsed.lfWidth = value[1];
    parsed.lfEscapement = value[2];
    parsed.lfOrientation = value[3];
    parsed.lfWeight = value[4];
    parsed.lfItalic = static_cast<BYTE>(value[5]);
    parsed.lfUnderline = static_cast<BYTE>(value[6]);
    parsed.lfStrikeOut = static_cast<BYTE>(value[7]);
    parsed.lfCharSet = static_cast<BYTE>(value[8]);
    parsed.lfOutPrecision = static_cast<BYTE>(value[9]);
    parsed.lfClipPrecision = static_cast<BYTE>(value[10]);
    parsed.lfQuality = static_cast<BYTE>(value[11]);
    parsed.lfPitchAndFamily = static_cast<BYTE>(value[12]);
    // FUN_0048DE50 always copies the final field, including an empty face
    // name.  This intentionally clears the seeded face while preserving all
    // thirteen numeric LOGFONT members.
    wcsncpy_s(parsed.lfFaceName, descriptor.c_str() + comma, _TRUNCATE);
    font = parsed;
    return true;
}

void LoadLyricColors(const std::filesystem::path& directory, LyricSkin& lyric) {
    InitializeDefaultLogFont(lyric.font);
    const auto path = directory / L"Lyric.xml";
    if (!std::filesystem::exists(path)) return;
    ComScope com;
    IXMLDOMDocument* document{};
    if (FAILED(CoCreateInstance(__uuidof(DOMDocument60), nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&document)))) return;
    document->put_async(VARIANT_FALSE);
    VARIANT_BOOL loaded{};
    try {
        const auto xml = DecodeLegacyXml(path);
        document->loadXML(_bstr_t(xml.c_str()), &loaded);
    } catch (const std::exception&) {
        document->Release();
        return;
    }
    if (loaded == VARIANT_TRUE) {
        IXMLDOMNode* node{};
        document->selectSingleNode(_bstr_t(L"/ttplayer_lyric/Lyric"), &node);
        if (node) {
            static_cast<void>(ParseLogFont(Attribute(node, L"Font"), lyric.font));
            lyric.text_color = ParseColor(Attribute(node, L"TextColor"),
                                          lyric.text_color);
            lyric.highlight_color = ParseColor(Attribute(node, L"HilightColor"),
                                               lyric.highlight_color);
            lyric.background_color = ParseColor(Attribute(node, L"BkgndColor"),
                                                lyric.background_color);
            node->Release();
        }
    }
    document->Release();
}

void LoadPlaylistColors(const std::filesystem::path& directory, PlaylistSkin& playlist) {
    const auto path = directory / L"Playlist.xml";
    if (!std::filesystem::exists(path)) return;
    ComScope com;
    IXMLDOMDocument* document{};
    if (FAILED(CoCreateInstance(__uuidof(DOMDocument60), nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&document)))) return;
    document->put_async(VARIANT_FALSE);
    VARIANT_BOOL loaded{};
    try {
        const auto xml = DecodeLegacyXml(path);
        document->loadXML(_bstr_t(xml.c_str()), &loaded);
    } catch (const std::exception&) {
        document->Release();
        return;
    }
    if (loaded == VARIANT_TRUE) {
        IXMLDOMNode* node{};
        document->selectSingleNode(_bstr_t(L"/ttplayer_playlist/PlayList"), &node);
        if (node) {
            const auto descriptor = Attribute(node, L"Font");
            if (!descriptor.empty()) {
                const auto comma = descriptor.rfind(L',');
                if (comma != std::wstring::npos && comma + 1 < descriptor.size())
                    playlist.font = descriptor.substr(comma + 1);
                const auto first_comma = descriptor.find(L',');
                playlist.font_height = ParseInt(descriptor.substr(0, first_comma),
                                                playlist.font_height);
            }
            playlist.text_color = ParseColor(Attribute(node, L"Color_Text"), playlist.text_color);
            playlist.highlight_color = ParseColor(Attribute(node, L"Color_Hilight"), playlist.highlight_color);
            playlist.background_color = ParseColor(Attribute(node, L"Color_Bkgnd"), playlist.background_color);
            playlist.number_color = ParseColor(Attribute(node, L"Color_Number"), playlist.number_color);
            playlist.duration_color = ParseColor(Attribute(node, L"Color_Duration"), playlist.duration_color);
            // 0048E696 has a special fallback for an existing PlayList node:
            // absent/invalid Color_Select uses the system selection color.
            // An absent node/file, like other missing fields, retains the seed.
            playlist.selected_color = ParseColor(Attribute(node, L"Color_Select"),
                                                  GetSysColor(COLOR_HIGHLIGHT));
            const auto selected_text = Attribute(node, L"Color_SelText");
            if (!selected_text.empty()) {
                const auto color = ParseColor(selected_text, CLR_INVALID);
                if (color != CLR_INVALID) playlist.selected_text_color = color;
            }
            playlist.alternate_background_color = ParseColor(
                Attribute(node, L"Color_Bkgnd2"), playlist.alternate_background_color);
            node->Release();
        }
    }
    document->Release();
}

void LoadVisualSettings(const std::filesystem::path& directory,
                        VisualSkin& visual) {
    const auto path = directory / L"Visual.xml";
    if (!std::filesystem::exists(path)) return;
    ComScope com;
    IXMLDOMDocument* document{};
    if (FAILED(CoCreateInstance(__uuidof(DOMDocument60), nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&document))) || !document)
        return;
    document->put_async(VARIANT_FALSE);
    VARIANT_BOOL loaded{};
    try {
        const auto xml = DecodeLegacyXml(path);
        document->loadXML(_bstr_t(xml.c_str()), &loaded);
    } catch (const std::exception&) {
        document->Release();
        return;
    }
    if (loaded == VARIANT_TRUE) {
        IXMLDOMNode* node{};
        document->selectSingleNode(_bstr_t(L"/ttplayer_visual/Visual"), &node);
        if (node) {
            const auto color = [&](const wchar_t* name,
                                   std::optional<COLORREF>& destination) {
                const auto source = Attribute(node, name);
                if (source.empty()) return;
                const COLORREF parsed = ParseColor(source, CLR_INVALID);
                if (parsed != CLR_INVALID) destination = parsed;
            };
            const auto integer = [&](const wchar_t* name,
                                     std::optional<int>& destination) {
                const auto source = Attribute(node, name);
                if (!source.empty()) destination = ParseInt(source, 0);
            };
            color(L"SpectrumTopColor", visual.spectrum_top_color);
            color(L"SpectrumBtmColor", visual.spectrum_bottom_color);
            color(L"SpectrumMidColor", visual.spectrum_middle_color);
            color(L"SpectrumPeakColor", visual.spectrum_peak_color);
            integer(L"SpectrumWide", visual.spectrum_wide);
            integer(L"BlurSpeed", visual.blur_speed);
            integer(L"Type", visual.type);
            const auto blur = Attribute(node, L"Blur");
            if (!blur.empty()) visual.blur = ParseBool(blur);
            color(L"BlurScopeColor", visual.blur_scope_color);
            color(L"TextColor", visual.text_color);
            const auto descriptor = Attribute(node, L"Font");
            if (!descriptor.empty()) {
                LOGFONTW font{};
                if (ParseLogFont(descriptor, font)) visual.font = font;
            }
            visual.valid = true;
            node->Release();
        }
    }
    document->Release();
}
} // namespace

LegacySkin LegacySkin::Load(const std::filesystem::path& directory,
                            const settings::Settings* current) {
    ComScope com;
    if (FAILED(com.result) && com.result != RPC_E_CHANGED_MODE) throw std::runtime_error("COM init failed");
    IXMLDOMDocument* document{};
    if (FAILED(CoCreateInstance(__uuidof(DOMDocument60), nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&document)))) {
        throw std::runtime_error("MSXML6 unavailable");
    }
    document->put_async(VARIANT_FALSE);
    VARIANT_BOOL loaded{};
    auto xml = DecodeLegacyXml(directory / L"Skin.xml");
    // The original streaming XML reader stops at </skin>. Several published
    // packages append an old coordinate line after the root element (for
    // example ironger-BlueArmorPlayer); MSXML rejects that otherwise valid
    // package as top-level trailing content.
    if (const size_t end = xml.find(L"</skin>"); end != std::wstring::npos)
        xml.erase(end + std::wstring_view(L"</skin>").size());
    document->loadXML(_bstr_t(xml.c_str()), &loaded);
    if (loaded != VARIANT_TRUE) {
        IXMLDOMParseError* parse_error{};
        std::string detail;
        if (SUCCEEDED(document->get_parseError(&parse_error)) && parse_error) {
            BSTR reason{};
            parse_error->get_reason(&reason);
            if (reason) {
                const int length = WideCharToMultiByte(CP_UTF8, 0, reason, SysStringLen(reason),
                    nullptr, 0, nullptr, nullptr);
                detail.resize(static_cast<size_t>(length));
                WideCharToMultiByte(CP_UTF8, 0, reason, SysStringLen(reason), detail.data(), length,
                    nullptr, nullptr);
                SysFreeString(reason);
            }
            long line{};
            long position{};
            parse_error->get_line(&line);
            parse_error->get_linepos(&position);
            if (line > 0) detail += " (line " + std::to_string(line) +
                ", column " + std::to_string(position) + ")";
            if (line == 1 && position > 0) {
                const size_t begin = static_cast<size_t>(std::max(0L, position - 16));
                const size_t length = std::min<size_t>(48, xml.size() -
                    std::min(begin, xml.size()));
                std::string context;
                context.reserve(length);
                for (const wchar_t value : xml.substr(begin, length))
                    context.push_back(value < 0x80 ? static_cast<char>(value) : '?');
                detail += " near `" + context + "`";
            }
            parse_error->Release();
        }
        document->Release();
        throw std::runtime_error("invalid Skin.xml: " + detail);
    }

    LegacySkin skin;
    // 0048E635 / 0048EA53 seed package descriptors from the current settings
    // before parsing sparse XML. Standalone loads start with DLL defaults.
    if (current) {
        auto& list = skin.playlist_;
        list.text_color = current->playlist.text_color;
        list.highlight_color = current->playlist.highlight_color;
        list.number_color = current->playlist.number_color;
        list.duration_color = current->playlist.duration_color;
        list.selected_color = current->playlist.selected_color;
        list.background_color = current->playlist.background_color;
        list.alternate_background_color = current->playlist.alternate_background_color;
        auto& lyric = skin.lyric_;
        if (current->lyric.text_color != CLR_INVALID)
            lyric.text_color = current->lyric.text_color;
        if (current->lyric.highlight_color != CLR_INVALID)
            lyric.highlight_color = current->lyric.highlight_color;
        if (current->lyric.background_color != CLR_INVALID)
            lyric.background_color = current->lyric.background_color;
    }
    IXMLDOMNode* root{};
    document->selectSingleNode(_bstr_t(L"/skin"), &root);
    skin.transparent_color_ = ParseColor(Attribute(root, L"transparent_color"), RGB(255, 0, 255));
    if (root) root->Release();

    IXMLDOMNode* player{};
    document->selectSingleNode(_bstr_t(L"/skin/player_window"), &player);
    if (!player) {
        document->Release();
        throw std::runtime_error("player_window missing from Skin.xml");
    }
    const auto background_name = Attribute(player, L"image");
    skin.background_ = skin.LoadBitmap(directory / background_name);
    if (!skin.background_) {
        player->Release();
        document->Release();
        throw std::runtime_error("player skin bitmap unavailable");
    }
    BITMAP background_info{};
    GetObjectW(skin.background_, sizeof(background_info), &background_info);
    skin.window_size_ = {background_info.bmWidth, background_info.bmHeight};

    IXMLDOMNodeList* children{};
    player->get_childNodes(&children);
    long count{};
    if (children) children->get_length(&count);
    for (long index = 0; index < count; ++index) {
        IXMLDOMNode* node{};
        children->get_item(index, &node);
        if (!node) continue;
        DOMNodeType type{};
        node->get_nodeType(&type);
        if (type != NODE_ELEMENT) { node->Release(); continue; }
        BSTR raw_name{};
        node->get_nodeName(&raw_name);
        SkinElement element;
        if (raw_name) {
            element.name.assign(raw_name, SysStringLen(raw_name));
            SysFreeString(raw_name);
        }
        element.four_state = IsFourStateButton(element.name);
        element.bounds = ParseRect(Attribute(node, L"position"));
        element.color = ParseColor(Attribute(node, L"color"), element.color);
        element.background = ParseColor(Attribute(node, L"bkgnd"), element.background);
        element.alignment = ParseAlignment(Attribute(node, L"align"));
        const auto font = Attribute(node, L"font");
        if (!font.empty()) element.font = font;
        element.font_size = ParseInt(Attribute(node, L"font_size"), element.font_size);
        element.vertical = _wcsicmp(Attribute(node, L"vertical").c_str(), L"true") == 0;
        const auto image = Attribute(node, L"image");
        if (!image.empty()) {
            element.image = skin.LoadBitmap(directory / image);
            if (element.image) {
                BITMAP info{};
                GetObjectW(element.image, sizeof(info), &info);
                element.image_size = {info.bmWidth, info.bmHeight};
                if (element.four_state) {
                    // FUN_0040A35B always treats a button strip as exactly four
                    // horizontal frames and sizes the child to frameWidth x
                    // bitmapHeight, rather than stretching to the XML rect.
                    element.frames = 4;
                    element.bounds.right = element.bounds.left + info.bmWidth / 4;
                    element.bounds.bottom = element.bounds.top + info.bmHeight;
                }
            }
        }
        if (element.name == L"icon" && !skin.icon_) {
            auto icon_name = Attribute(node, L"icon");
            if (!icon_name.empty()) {
                skin.icon_ = static_cast<HICON>(LoadImageW(nullptr,
                    (directory / icon_name).c_str(), IMAGE_ICON,
                    GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                    LR_LOADFROMFILE));
            }
        }
        const auto fill_image = Attribute(node, L"fill_image");
        if (!fill_image.empty()) {
            element.fill_image = skin.LoadBitmap(directory / fill_image);
            if (element.fill_image) {
                BITMAP info{};
                GetObjectW(element.fill_image, sizeof(info), &info);
                element.fill_size = {info.bmWidth, info.bmHeight};
            }
        }
        const auto fill_image2 = Attribute(node, L"fill_image2");
        if (!fill_image2.empty()) {
            element.fill_image2 = skin.LoadBitmap(directory / fill_image2);
            if (element.fill_image2) {
                BITMAP info{};
                GetObjectW(element.fill_image2, sizeof(info), &info);
                element.fill_size2 = {info.bmWidth, info.bmHeight};
            }
        }
        const auto bar_image = Attribute(node, L"bar_image");
        if (!bar_image.empty()) {
            element.bar_image = skin.LoadBitmap(directory / bar_image);
            if (element.bar_image) {
                BITMAP info{};
                GetObjectW(element.bar_image, sizeof(info), &info);
                element.bar_size = {info.bmWidth, info.bmHeight};
            }
        }
        const auto thumb_image = Attribute(node, L"thumb_image");
        if (!thumb_image.empty()) {
            element.thumb_image = skin.LoadBitmap(directory / thumb_image);
            if (element.thumb_image) {
                BITMAP info{};
                GetObjectW(element.thumb_image, sizeof(info), &info);
                element.thumb_size = {info.bmWidth, info.bmHeight};
            }
        }
        LoadElementAnimation(skin, directory, node, element);
        skin.elements_.push_back(std::move(element));
        node->Release();
    }
    if (children) children->Release();
    player->Release();

    // FUN_0047EBxx stores mini_window at skin-object offset +0x4A0.  The
    // double-click handler (00460CF5) enables command 0x7DD4 only when this
    // layout exists, and 00464B6C swaps the live controls to these elements.
    IXMLDOMNode* mini_window{};
    document->selectSingleNode(_bstr_t(L"/skin/mini_window"), &mini_window);
    if (mini_window) {
        const auto mini_background = Attribute(mini_window, L"image");
        skin.mini_.background = skin.LoadBitmap(directory / mini_background);
        if (skin.mini_.background) {
            BITMAP mini_info{};
            GetObjectW(skin.mini_.background, sizeof(mini_info), &mini_info);
            skin.mini_.window_size = {mini_info.bmWidth, mini_info.bmHeight};

            IXMLDOMNodeList* mini_children{};
            mini_window->get_childNodes(&mini_children);
            long mini_count{};
            if (mini_children) mini_children->get_length(&mini_count);
            for (long index = 0; index < mini_count; ++index) {
                IXMLDOMNode* node{};
                mini_children->get_item(index, &node);
                if (!node) continue;
                DOMNodeType type{};
                node->get_nodeType(&type);
                if (type != NODE_ELEMENT) { node->Release(); continue; }

                BSTR raw_name{};
                node->get_nodeName(&raw_name);
                SkinElement element;
                if (raw_name) {
                    element.name.assign(raw_name, SysStringLen(raw_name));
                    SysFreeString(raw_name);
                }
                element.four_state = IsFourStateButton(element.name);
                element.bounds = ParseRect(Attribute(node, L"position"));
                element.color = ParseColor(Attribute(node, L"color"), element.color);
                element.background = ParseColor(Attribute(node, L"bkgnd"), element.background);
                element.alignment = ParseAlignment(Attribute(node, L"align"));
                const auto font = Attribute(node, L"font");
                if (!font.empty()) element.font = font;
                element.font_size = ParseInt(Attribute(node, L"font_size"), element.font_size);
                element.vertical = _wcsicmp(Attribute(node, L"vertical").c_str(), L"true") == 0;

                const auto image = Attribute(node, L"image");
                if (!image.empty()) {
                    element.image = skin.LoadBitmap(directory / image);
                    if (element.image) {
                        BITMAP info{};
                        GetObjectW(element.image, sizeof(info), &info);
                        element.image_size = {info.bmWidth, info.bmHeight};
                        if (element.four_state) {
                            element.frames = 4;
                            element.bounds.right = element.bounds.left + info.bmWidth / 4;
                            element.bounds.bottom = element.bounds.top + info.bmHeight;
                        }
                    }
                }
                const auto fill_image = Attribute(node, L"fill_image");
                if (!fill_image.empty()) {
                    element.fill_image = skin.LoadBitmap(directory / fill_image);
                    if (element.fill_image) {
                        BITMAP info{};
                        GetObjectW(element.fill_image, sizeof(info), &info);
                        element.fill_size = {info.bmWidth, info.bmHeight};
                    }
                }
                const auto fill_image2 = Attribute(node, L"fill_image2");
                if (!fill_image2.empty()) {
                    element.fill_image2 = skin.LoadBitmap(directory / fill_image2);
                    if (element.fill_image2) {
                        BITMAP info{};
                        GetObjectW(element.fill_image2, sizeof(info), &info);
                        element.fill_size2 = {info.bmWidth, info.bmHeight};
                    }
                }
                const auto bar_image = Attribute(node, L"bar_image");
                if (!bar_image.empty()) {
                    element.bar_image = skin.LoadBitmap(directory / bar_image);
                    if (element.bar_image) {
                        BITMAP info{};
                        GetObjectW(element.bar_image, sizeof(info), &info);
                        element.bar_size = {info.bmWidth, info.bmHeight};
                    }
                }
                const auto thumb_image = Attribute(node, L"thumb_image");
                if (!thumb_image.empty()) {
                    element.thumb_image = skin.LoadBitmap(directory / thumb_image);
                    if (element.thumb_image) {
                        BITMAP info{};
                        GetObjectW(element.thumb_image, sizeof(info), &info);
                        element.thumb_size = {info.bmWidth, info.bmHeight};
                    }
                }
                LoadElementAnimation(skin, directory, node, element);
                skin.mini_.elements.push_back(std::move(element));
                node->Release();
            }
            if (mini_children) mini_children->Release();
        }
        mini_window->Release();
    }

    // 004A88D0 stores lyric_window at skin-object offset +0x91C.  Its three
    // commands are real four-frame SkinButton children and its lyric rect is
    // enlarged by the same delta as the resizable popup.
    IXMLDOMNode* lyric_window{};
    document->selectSingleNode(_bstr_t(L"/skin/lyric_window"), &lyric_window);
    if (lyric_window) {
        auto& lyric = skin.lyric_;
        lyric.position = ParseRect(Attribute(lyric_window, L"position"));
        lyric.resize_rect = ParseRect(Attribute(lyric_window, L"resize_rect"));
        lyric.resize_tile = ParseBool(Attribute(lyric_window, L"resize_tile"));
        lyric.background = LoadSkinBitmap(skin, directory, lyric_window, L"image");
        lyric.valid = lyric.background.image != nullptr;

        IXMLDOMNodeList* lyric_children{};
        lyric_window->get_childNodes(&lyric_children);
        long lyric_count{};
        if (lyric_children) lyric_children->get_length(&lyric_count);
        for (long index = 0; index < lyric_count; ++index) {
            IXMLDOMNode* node{};
            lyric_children->get_item(index, &node);
            if (!node) continue;
            DOMNodeType type{};
            node->get_nodeType(&type);
            if (type != NODE_ELEMENT) { node->Release(); continue; }
            BSTR raw_name{};
            node->get_nodeName(&raw_name);
            const std::wstring name = raw_name
                ? std::wstring(raw_name, SysStringLen(raw_name)) : std::wstring{};
            if (raw_name) SysFreeString(raw_name);
            if (_wcsicmp(name.c_str(), L"title") == 0) {
                lyric.title = LoadEqualizerElement(skin, directory, node, name, false);
            } else if (_wcsicmp(name.c_str(), L"close") == 0) {
                lyric.close = LoadEqualizerElement(skin, directory, node, name, true);
            } else if (_wcsicmp(name.c_str(), L"ontop") == 0) {
                lyric.ontop = LoadEqualizerElement(skin, directory, node, name, true);
            } else if (_wcsicmp(name.c_str(), L"desklrc") == 0) {
                lyric.desklrc = LoadEqualizerElement(skin, directory, node, name, true);
            } else if (_wcsicmp(name.c_str(), L"lyric") == 0) {
                lyric.lyric_bounds = ParseRect(Attribute(node, L"position"));
            } else if (_wcsicmp(name.c_str(), L"mini_border") == 0) {
                lyric.mini_border_left_top = ParseColor(
                    Attribute(node, L"left_top_color"), 0xff000000);
                lyric.mini_border_right_bottom = ParseColor(
                    Attribute(node, L"right_bottom_color"), 0xff000000);
            } else if (_wcsicmp(name.c_str(), L"mini_lyric") == 0) {
                // Compatibility extension for converted dialog-based skins;
                // not a node understood by the original 5.7.9 parser.
                InitializeDefaultLogFont(lyric.mini_font);
                lyric.mini_font_valid = ParseLogFont(
                    Attribute(node, L"Font"), lyric.mini_font);
                lyric.mini_text_color = ParseColor(Attribute(node, L"TextColor"), CLR_INVALID);
                lyric.mini_highlight_color = ParseColor(Attribute(node, L"HilightColor"), CLR_INVALID);
                lyric.mini_background_color = ParseColor(Attribute(node, L"BkgndColor"), CLR_INVALID);
                RECT padding{};
                const auto value = Attribute(node, L"padding");
                if (swscanf_s(value.c_str(), L"%ld , %ld , %ld , %ld",
                        &padding.left, &padding.top, &padding.right, &padding.bottom) == 4 &&
                    padding.left >= 0 && padding.top >= 0 &&
                    padding.right >= 0 && padding.bottom >= 0 &&
                    padding.left <= 1024 && padding.top <= 1024 &&
                    padding.right <= 1024 && padding.bottom <= 1024)
                    lyric.mini_padding = padding;
            }
            node->Release();
        }
        if (lyric_children) lyric_children->Release();
        lyric_window->Release();
        LoadLyricColors(directory, lyric);
    }

    // 004A92A2 parses /skin/desklrc_bar after the ordinary lyric window.
    // Unlike the main skin controls its XML rectangles are retained verbatim:
    // FUN_004186BC installs the four-frame images and then applies positions.
    IXMLDOMNode* desktop_bar_node{};
    document->selectSingleNode(_bstr_t(L"/skin/desklrc_bar"),
                               &desktop_bar_node);
    if (desktop_bar_node) {
        auto& bar = skin.desktop_lyric_bar_;
        bar.background = LoadSkinBitmap(skin, directory, desktop_bar_node,
                                        L"image");
        bar.transparent_color = ParseColor(
            Attribute(desktop_bar_node, L"transparent_color"),
            RGB(255, 0, 255));
        bar.valid = bar.background.image != nullptr;

        IXMLDOMNodeList* bar_children{};
        desktop_bar_node->get_childNodes(&bar_children);
        long bar_count{};
        if (bar_children) bar_children->get_length(&bar_count);
        for (long index = 0; index < bar_count; ++index) {
            IXMLDOMNode* node{};
            bar_children->get_item(index, &node);
            if (!node) continue;
            DOMNodeType type{};
            node->get_nodeType(&type);
            if (type != NODE_ELEMENT) {
                node->Release();
                continue;
            }
            BSTR raw_name{};
            node->get_nodeName(&raw_name);
            const std::wstring name = raw_name
                ? std::wstring(raw_name, SysStringLen(raw_name))
                : std::wstring{};
            if (raw_name) SysFreeString(raw_name);

            if (_wcsicmp(name.c_str(), L"icon") == 0) {
                bar.icon = ParseRect(Attribute(node, L"position"));
            } else {
                SkinElement element = LoadEqualizerElement(
                    skin, directory, node, name, true);
                if (_wcsicmp(name.c_str(), L"play") == 0)
                    bar.play = std::move(element);
                else if (_wcsicmp(name.c_str(), L"pause") == 0)
                    bar.pause = std::move(element);
                else if (_wcsicmp(name.c_str(), L"prev") == 0)
                    bar.previous = std::move(element);
                else if (_wcsicmp(name.c_str(), L"next") == 0)
                    bar.next = std::move(element);
                else if (_wcsicmp(name.c_str(), L"list") == 0)
                    bar.list = std::move(element);
                else if (_wcsicmp(name.c_str(), L"settings") == 0)
                    bar.settings = std::move(element);
                else if (_wcsicmp(name.c_str(), L"kalaok") == 0)
                    bar.karaoke = std::move(element);
                else if (_wcsicmp(name.c_str(), L"lines") == 0)
                    bar.lines = std::move(element);
                else if (_wcsicmp(name.c_str(), L"lock") == 0)
                    bar.lock = std::move(element);
                else if (_wcsicmp(name.c_str(), L"ontop") == 0)
                    bar.ontop = std::move(element);
                else if (_wcsicmp(name.c_str(), L"zoomin") == 0)
                    bar.zoom_in = std::move(element);
                else if (_wcsicmp(name.c_str(), L"zoomout") == 0)
                    bar.zoom_out = std::move(element);
                else if (_wcsicmp(name.c_str(), L"return") == 0)
                    bar.return_to_window = std::move(element);
                else if (_wcsicmp(name.c_str(), L"close") == 0)
                    bar.close = std::move(element);
            }
            node->Release();
        }
        if (bar_children) bar_children->Release();
        desktop_bar_node->Release();
    }

    // CSkinParser_ParseEqualizerWindow (004A8B73) dispatches these exact
    // node names.  In particular eqfactor is not a single control in the
    // native object: FUN_0042955B materializes ten copies at width+interval.
    IXMLDOMNode* equalizer_window{};
    document->selectSingleNode(_bstr_t(L"/skin/equalizer_window"), &equalizer_window);
    if (equalizer_window) {
        auto& equalizer = skin.equalizer_;
        equalizer.position = ParseRect(Attribute(equalizer_window, L"position"));
        equalizer.eq_interval = ParseInt(Attribute(equalizer_window, L"eq_interval"), 0);
        equalizer.background = LoadSkinBitmap(skin, directory, equalizer_window, L"image");
        equalizer.valid = equalizer.background.image != nullptr;

        IXMLDOMNodeList* equalizer_children{};
        equalizer_window->get_childNodes(&equalizer_children);
        long equalizer_count{};
        if (equalizer_children) equalizer_children->get_length(&equalizer_count);
        for (long index = 0; index < equalizer_count; ++index) {
            IXMLDOMNode* node{};
            equalizer_children->get_item(index, &node);
            if (!node) continue;
            DOMNodeType type{};
            node->get_nodeType(&type);
            if (type != NODE_ELEMENT) { node->Release(); continue; }
            BSTR raw_name{};
            node->get_nodeName(&raw_name);
            const std::wstring name = raw_name
                ? std::wstring(raw_name, SysStringLen(raw_name)) : std::wstring{};
            if (raw_name) SysFreeString(raw_name);
            if (_wcsicmp(name.c_str(), L"title") == 0) {
                equalizer.title = LoadEqualizerElement(skin, directory, node, name, false);
            } else if (_wcsicmp(name.c_str(), L"close") == 0) {
                equalizer.close = LoadEqualizerElement(skin, directory, node, name, true);
            } else if (_wcsicmp(name.c_str(), L"enabled") == 0) {
                equalizer.enabled = LoadEqualizerElement(skin, directory, node, name, true);
            } else if (_wcsicmp(name.c_str(), L"profile") == 0) {
                equalizer.profile = LoadEqualizerElement(skin, directory, node, name, true);
            } else if (_wcsicmp(name.c_str(), L"reset") == 0) {
                equalizer.reset = LoadEqualizerElement(skin, directory, node, name, true);
            } else if (_wcsicmp(name.c_str(), L"balance") == 0) {
                equalizer.balance = LoadEqualizerElement(skin, directory, node, name, false);
            } else if (_wcsicmp(name.c_str(), L"surround") == 0) {
                equalizer.surround = LoadEqualizerElement(skin, directory, node, name, false);
            } else if (_wcsicmp(name.c_str(), L"preamp") == 0) {
                equalizer.preamp = LoadEqualizerElement(skin, directory, node, name, false, true);
            } else if (_wcsicmp(name.c_str(), L"eqfactor") == 0) {
                auto band = LoadEqualizerElement(skin, directory, node, name, false, true);
                const LONG stride = (band.bounds.right - band.bounds.left) +
                                    equalizer.eq_interval;
                for (size_t band_index = 0; band_index < equalizer.bands.size(); ++band_index) {
                    equalizer.bands[band_index] = band;
                    equalizer.bands[band_index].name =
                        L"eqfactor" + std::to_wstring(band_index);
                    OffsetRect(&equalizer.bands[band_index].bounds,
                               stride * static_cast<LONG>(band_index), 0);
                }
            }
            node->Release();
        }
        if (equalizer_children) equalizer_children->Release();
        equalizer_window->Release();
    }

    // 004A8DC6 parses playlist_window independently from player_window.  It
    // is deliberately loaded before the XML document is released so runtime
    // skin switching can update both HWNDs transactionally.
    IXMLDOMNode* playlist_window{};
    document->selectSingleNode(_bstr_t(L"/skin/playlist_window"), &playlist_window);
    if (playlist_window) {
        auto& playlist = skin.playlist_;
        playlist.position = ParseRect(Attribute(playlist_window, L"position"));
        playlist.resize_rect = ParseRect(Attribute(playlist_window, L"resize_rect"));
        playlist.resize_tile = ParseBool(Attribute(playlist_window, L"resize_tile"));
        playlist.background = LoadSkinBitmap(skin, directory, playlist_window, L"image");
        playlist.valid = playlist.background.image != nullptr;

        IXMLDOMNodeList* playlist_children{};
        playlist_window->get_childNodes(&playlist_children);
        long playlist_count{};
        if (playlist_children) playlist_children->get_length(&playlist_count);
        for (long index = 0; index < playlist_count; ++index) {
            IXMLDOMNode* node{};
            playlist_children->get_item(index, &node);
            if (!node) continue;
            DOMNodeType type{};
            node->get_nodeType(&type);
            if (type != NODE_ELEMENT) { node->Release(); continue; }
            BSTR raw_name{};
            node->get_nodeName(&raw_name);
            const std::wstring name = raw_name
                ? std::wstring(raw_name, SysStringLen(raw_name)) : std::wstring{};
            if (raw_name) SysFreeString(raw_name);
            if (_wcsicmp(name.c_str(), L"title") == 0) {
                playlist.title = LoadPlaylistImageElement(skin, directory, node, L"title");
            } else if (_wcsicmp(name.c_str(), L"close") == 0) {
                playlist.close = LoadPlaylistImageElement(skin, directory, node, L"close");
            } else if (_wcsicmp(name.c_str(), L"toolbar") == 0) {
                playlist.toolbar_bounds = ParseRect(Attribute(node, L"position"));
                playlist.toolbar_alignment = ParseAlignment(Attribute(node, L"align"));
                playlist.toolbar = LoadSkinBitmap(skin, directory, node, L"image");
                playlist.toolbar_hot = LoadSkinBitmap(skin, directory, node, L"hot_image");
                playlist.toolbar_animation = LoadAnimation(node, 1);
                IXMLDOMNodeList* items{};
                node->selectNodes(_bstr_t(L"item"), &items);
                long toolbar_item_count{};
                if (items) items->get_length(&toolbar_item_count);
                for (long item_index = 0; item_index < toolbar_item_count; ++item_index) {
                    IXMLDOMNode* item{};
                    items->get_item(item_index, &item);
                    if (!item) continue;
                    const int slot = ParseInt(Attribute(item, L"index"), -1);
                    const RECT rect = ParseRect(Attribute(item, L"position"));
                    if (slot >= 0 && slot < 7 && rect.left >= 0 && rect.top >= 0 &&
                        rect.right > rect.left && rect.bottom > rect.top &&
                        rect.right <= playlist.toolbar_bounds.right - playlist.toolbar_bounds.left &&
                        rect.bottom <= playlist.toolbar_bounds.bottom - playlist.toolbar_bounds.top) {
                        if (!playlist.toolbar_items)
                            playlist.toolbar_items.emplace(std::array<RECT, 7>{});
                        (*playlist.toolbar_items)[slot] = rect;
                    }
                    item->Release();
                }
                if (items) items->Release();
            } else if (_wcsicmp(name.c_str(), L"scrollbar") == 0) {
                playlist.scrollbar_buttons = LoadSkinBitmap(skin, directory, node, L"buttons_image");
                playlist.scrollbar_thumb = LoadSkinBitmap(skin, directory, node, L"thumb_image");
                playlist.scrollbar_bar = LoadSkinBitmap(skin, directory, node, L"bar_image");
                playlist.scrollbar_thumb_resize_center = std::max(0, ParseInt(
                    Attribute(node, L"thumb_resize_center"), 0));
                playlist.scrollbar_thumb_resize_tile = ParseBool(
                    Attribute(node, L"thumb_resize_tile"));
            } else if (_wcsicmp(name.c_str(), L"playlist") == 0) {
                playlist.list_bounds = ParseRect(Attribute(node, L"position"));
                playlist.splitter_bar = LoadSkinBitmap(skin, directory, node, L"splitter_bar_image");
                playlist.splitter_arrow = LoadSkinBitmap(skin, directory, node, L"splitter_arrow_image");
                playlist.selected = LoadSkinBitmap(skin, directory, node, L"selected_image");
            }
            node->Release();
        }
        if (playlist_children) playlist_children->Release();
        playlist_window->Release();
        LoadPlaylistColors(directory, playlist);
    }

    // CSkinParser_ParseBrowserWindow：原版「查看 → 音乐窗」的内嵌浏览器框体。
    // 与 equalizer/playlist 一样是主窗口拥有的固定尺寸弹窗——没有 resize_rect，
    // 因此窗口尺寸就是背景位图尺寸，内容区完全由 <browser position> 给出。
    IXMLDOMNode* browser_window{};
    document->selectSingleNode(_bstr_t(L"/skin/browser_window"), &browser_window);
    if (browser_window) {
        auto& browser = skin.browser_;
        browser.position = ParseRect(Attribute(browser_window, L"position"));
        browser.transparent_color = ParseColor(
            Attribute(browser_window, L"transparent_color"), CLR_INVALID);
        browser.background = LoadSkinBitmap(skin, directory, browser_window, L"image");
        browser.valid = browser.background.image != nullptr;

        IXMLDOMNodeList* browser_children{};
        browser_window->get_childNodes(&browser_children);
        long browser_count{};
        if (browser_children) browser_children->get_length(&browser_count);
        for (long index = 0; index < browser_count; ++index) {
            IXMLDOMNode* node{};
            browser_children->get_item(index, &node);
            if (!node) continue;
            DOMNodeType type{};
            node->get_nodeType(&type);
            if (type != NODE_ELEMENT) { node->Release(); continue; }
            BSTR raw_name{};
            node->get_nodeName(&raw_name);
            const std::wstring name = raw_name
                ? std::wstring(raw_name, SysStringLen(raw_name)) : std::wstring{};
            if (raw_name) SysFreeString(raw_name);
            if (_wcsicmp(name.c_str(), L"close") == 0) {
                browser.close = LoadBrowserButton(skin, directory, node, L"close");
            } else if (_wcsicmp(name.c_str(), L"backward") == 0) {
                browser.backward = LoadBrowserButton(skin, directory, node, L"backward");
            } else if (_wcsicmp(name.c_str(), L"forward") == 0) {
                browser.forward = LoadBrowserButton(skin, directory, node, L"forward");
            } else if (_wcsicmp(name.c_str(), L"refresh") == 0) {
                browser.refresh = LoadBrowserButton(skin, directory, node, L"refresh");
            } else if (_wcsicmp(name.c_str(), L"startup") == 0) {
                // ckbox_image 是四态复选框图，XML 矩形覆盖「勾选框 + 标签」整行；
                // 这里只取勾选框本身，标签由使用方自己画。
                browser.check = LoadBrowserButton(skin, directory, node, L"startup",
                                                  L"ckbox_image");
                browser.check_interval = ParseInt(Attribute(node, L"interval"), 0);
                browser.check.color = ParseColor(Attribute(node, L"color"),
                                                 browser.check.color);
                const auto font = Attribute(node, L"font");
                if (!font.empty()) browser.check.font = font;
                browser.check.font_size = ParseInt(Attribute(node, L"font_size"),
                                                   browser.check.font_size);
            } else if (_wcsicmp(name.c_str(), L"linktxt") == 0) {
                browser.link.name = L"linktxt";
                browser.link.bounds = ParseRect(Attribute(node, L"position"));
                browser.link.alignment = ParseAlignment(Attribute(node, L"align"));
                browser.link.color = ParseColor(Attribute(node, L"color"),
                                                browser.link.color);
                const auto face = Attribute(node, L"font");
                if (!face.empty()) browser.link.font = face;
                browser.link.font_size = ParseInt(Attribute(node, L"font_size"),
                                                  browser.link.font_size);
            } else if (_wcsicmp(name.c_str(), L"browser") == 0) {
                browser.content_bounds = ParseRect(Attribute(node, L"position"));
            }
            node->Release();
        }
        if (browser_children) browser_children->Release();
        browser_window->Release();
    }
    // CSkinManager_LoadPackageXml opens Visual.xml after the window layouts
    // and stores its sparse descriptor at skin-object offset +0x498.
    LoadVisualSettings(directory, skin.visual_);
    document->Release();
    return skin;
}

const SkinElement* LegacySkin::Find(std::wstring_view name) const {
    const auto found = std::find_if(elements_.begin(), elements_.end(), [&](const SkinElement& item) {
        return _wcsicmp(item.name.c_str(), std::wstring(name).c_str()) == 0;
    });
    return found == elements_.end() ? nullptr : &*found;
}

const SkinElement* LegacySkin::FindMini(std::wstring_view name) const {
    const auto found = std::find_if(mini_.elements.begin(), mini_.elements.end(),
        [&](const SkinElement& item) {
            return _wcsicmp(item.name.c_str(), std::wstring(name).c_str()) == 0;
        });
    return found == mini_.elements.end() ? nullptr : &*found;
}

// --------------------------------------------------------------- 框体绘制
namespace {

void DrawBitmapPatch(HDC target, const SkinImage& source, const RECT& destination,
                     const RECT& source_rect, bool tile, const ui::DpiScale& scale) {
    const int destination_width = destination.right - destination.left;
    const int destination_height = destination.bottom - destination.top;
    const int source_width = source_rect.right - source_rect.left;
    const int source_height = source_rect.bottom - source_rect.top;
    if (destination_width <= 0 || destination_height <= 0 ||
        source_width <= 0 || source_height <= 0) return;
    if (!tile) {
        source.Draw(target, destination.left, destination.top,
                    destination_width, destination_height,
                    source_rect.left, source_rect.top, source_width, source_height);
        return;
    }
    // Tiled centre: each repeated patch is scaled to its on-screen size, not
    // left at 1x source pixels (otherwise 200% showed a grid of 1px tiles).
    const int step_x = scale.X(source_width);
    const int step_y = scale.Y(source_height);
    const int saved = SaveDC(target);
    IntersectClipRect(target, destination.left, destination.top,
                      destination.right, destination.bottom);
    for (int y = destination.top; y < destination.bottom; y += step_y) {
        for (int x = destination.left; x < destination.right; x += step_x) {
            source.Draw(target, x, y, step_x, step_y,
                        source_rect.left, source_rect.top, source_width, source_height);
        }
    }
    RestoreDC(target, saved);
}

HRGN CreateColorKeyRegion(HBITMAP bitmap, int width, int height,
                          COLORREF transparent) {
    if (!bitmap || width <= 0 || height <= 0) return nullptr;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
    const HDC screen = GetDC(nullptr);
    const int rows = GetDIBits(screen, bitmap, 0, static_cast<UINT>(height),
                               pixels.data(), &info, DIB_RGB_COLORS);
    ReleaseDC(nullptr, screen);
    if (rows != height) return nullptr;

    HRGN result = CreateRectRgn(0, 0, 0, 0);
    if (!result) return nullptr;
    for (int y = 0; y < height; ++y) {
        int run = -1;
        for (int x = 0; x <= width; ++x) {
            bool opaque = false;
            if (x < width) {
                const size_t offset = (static_cast<size_t>(y) * width + x) * 4;
                const COLORREF color = RGB(pixels[offset + 2], pixels[offset + 1],
                                           pixels[offset]);
                opaque = color != transparent;
            }
            if (opaque && run < 0) run = x;
            if (!opaque && run >= 0) {
                const HRGN row = CreateRectRgn(run, y, x, y + 1);
                if (row) {
                    CombineRgn(result, result, row, RGN_OR);
                    DeleteObject(row);
                }
                run = -1;
            }
        }
    }
    return result;
}

} // namespace

void DrawFrameBackground(HDC target, const SkinBitmap& bitmap, RECT resize_rect,
                         int width, int height, bool tile,
                         const ui::DpiScale& scale) {
    if (!bitmap.image || bitmap.size.cx <= 0 || bitmap.size.cy <= 0 ||
        width <= 0 || height <= 0) return;
    const int n = bitmap.image.SourceScale();  // asset multiplier (@1x => 1)
    // The background's natural size in logical (96-DPI) pixels. For @Nx assets
    // the decoded bitmap is larger, so derive the logical size by dividing out
    // the multiplier; resize_rect from the XML is always logical.
    const int logical_w = n > 1 ? bitmap.size.cx / n : bitmap.size.cx;
    const int logical_h = n > 1 ? bitmap.size.cy / n : bitmap.size.cy;
    if (resize_rect.right <= resize_rect.left ||
        resize_rect.bottom <= resize_rect.top ||
        width < logical_w || height < logical_h) {
        // 没有可拉伸的中心区（或目标比原始位图更小）：整幅 1:1 缩放到目标。
        bitmap.image.Draw(target, 0, 0, width, height, 0, 0,
                          bitmap.size.cx, bitmap.size.cy);
        return;
    }

    resize_rect.left = std::clamp<LONG>(resize_rect.left, 0, logical_w);
    resize_rect.right = std::clamp<LONG>(resize_rect.right, resize_rect.left, logical_w);
    resize_rect.top = std::clamp<LONG>(resize_rect.top, 0, logical_h);
    resize_rect.bottom = std::clamp<LONG>(resize_rect.bottom, resize_rect.top, logical_h);
    // Source split uses logical resize_rect * asset multiplier N; destination
    // split uses logical resize_rect * display DPI. The two are independent
    // (docs/HIGH_DPI_SKIN_SPEC.md §4 invariant).
    const int source_left = resize_rect.left * n;
    const int source_right = resize_rect.right * n;
    const int source_top = resize_rect.top * n;
    const int source_bottom = resize_rect.bottom * n;
    const int destination_right = width - scale.X(logical_w - resize_rect.right);
    const int destination_bottom = height - scale.Y(logical_h - resize_rect.bottom);
    const int source_x[] = {0, source_left, source_right, bitmap.size.cx};
    const int source_y[] = {0, source_top, source_bottom, bitmap.size.cy};
    const int destination_x[] = {0, scale.X(resize_rect.left), destination_right, width};
    const int destination_y[] = {0, scale.Y(resize_rect.top), destination_bottom, height};

    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            const RECT source_patch{source_x[column], source_y[row],
                                    source_x[column + 1], source_y[row + 1]};
            const RECT destination_patch{destination_x[column], destination_y[row],
                                         destination_x[column + 1], destination_y[row + 1]};
            DrawBitmapPatch(target, bitmap.image, destination_patch, source_patch,
                            tile && (row == 1 || column == 1), scale);
        }
    }
}

HRGN CreateFrameRegion(const SkinBitmap& bitmap, RECT resize_rect,
                       int width, int height, bool tile, COLORREF transparent,
                       const ui::DpiScale& scale) {
    // 区域掩码必须和背景用完全相同的九宫格/平铺映射，否则拉伸后边框会错位。
    // PNG 的洋红像素不是 BMP 的颜色键，要改走 alpha 掩码。
    auto source = bitmap;
    if (bitmap.image.IsGdiPlus()) {
        source.image = bitmap.image.CoverageMask();
        transparent = RGB(0, 0, 0);
    }
    if (!source.image || width <= 0 || height <= 0) return nullptr;
    if (width == source.size.cx && height == source.size.cy)
        return CreateColorKeyRegion(source.image, width, height, transparent);
    const HDC screen = GetDC(nullptr);
    const HDC canvas = CreateCompatibleDC(screen);
    const HBITMAP rendered = CreateCompatibleBitmap(screen, width, height);
    ReleaseDC(nullptr, screen);
    HRGN region{};
    if (canvas && rendered) {
        const HGDIOBJ old = SelectObject(canvas, rendered);
        const RECT bounds{0, 0, width, height};
        const HBRUSH brush = CreateSolidBrush(transparent);
        FillRect(canvas, &bounds, brush);
        DeleteObject(brush);
        DrawFrameBackground(canvas, source, resize_rect, width, height, tile, scale);
        SelectObject(canvas, old);
        region = CreateColorKeyRegion(rendered, width, height, transparent);
    }
    if (rendered) DeleteObject(rendered);
    if (canvas) DeleteDC(canvas);
    return region;
}

void DrawFrameButton(HDC target, const SkinElement& element, RECT bounds,
                     int state, COLORREF transparent, const ui::DpiScale& scale) {
    if (!element.image || element.image_size.cx <= 0 ||
        element.image_size.cy <= 0) return;
    const int frames = std::max(1, element.frames);
    const int frame_width = element.image_size.cx / frames;
    if (frame_width <= 0) return;
    state = std::clamp(state, 0, frames - 1);
    // FUN_0040A35B copies a SkinButton frame at its native dimensions from
    // client origin (0,0); it does not stretch the frame to an XML rectangle
    // that contains one or two pixels of padding. Multi-frame buttons were
    // previously drawn at 1x regardless of DPI; here the destination uses the
    // on-screen scale while the source offset stays in source pixels (§2.4).
    const int destination_width = frames > 1 ? scale.X(frame_width)
                                             : bounds.right - bounds.left;
    const int destination_height = frames > 1 ? scale.Y(element.image_size.cy)
                                              : bounds.bottom - bounds.top;
    element.image.Draw(target, bounds.left, bounds.top, destination_width,
                       destination_height, state * frame_width, 0,
                       frame_width, element.image_size.cy, transparent);
}

// ----------------------------------------------------------------- 镜像缩放
SkinElement ScaleElement(const SkinElement& e, const ui::DpiScale& s) {
    SkinElement out = e;
    out.bounds = s.Rect(e.bounds);
    out.font_size = s.FontPx(e.font_size);
    return out;
}
std::vector<SkinElement> ScaleElements(const std::vector<SkinElement>& in,
                                       const ui::DpiScale& s) {
    std::vector<SkinElement> out;
    out.reserve(in.size());
    for (const auto& e : in) out.push_back(ScaleElement(e, s));
    return out;
}
void ScaleLogfont(LOGFONTW& lf, const ui::DpiScale& s) {
    lf.lfHeight = s.LfHeight(lf.lfHeight);  // 负值 em 高度须保号
    if (lf.lfWidth != 0) lf.lfWidth = s.LfHeight(lf.lfWidth);
}

LegacySkin LegacySkin::AtDpi(const ui::DpiScale& scale) const {
    LegacySkin out;
    out.transparent_color_ = transparent_color_;
    out.icon_ = icon_;
    out.window_size_ = scale.Size(window_size_);
    out.background_ = background_;
    out.bitmaps_ = bitmaps_;  // 共享 storage，不重新解码
    out.elements_ = ScaleElements(elements_, scale);

    out.mini_.background = mini_.background;
    out.mini_.window_size = scale.Size(mini_.window_size);
    out.mini_.elements = ScaleElements(mini_.elements, scale);

    out.playlist_ = playlist_;
    out.playlist_.position = scale.Rect(playlist_.position);
    out.playlist_.list_bounds = scale.Rect(playlist_.list_bounds);
    out.playlist_.toolbar_bounds = scale.Rect(playlist_.toolbar_bounds);
    out.playlist_.close = ScaleElement(playlist_.close, scale);
    out.playlist_.title = ScaleElement(playlist_.title, scale);
    if (playlist_.toolbar_items) {
        auto items = *playlist_.toolbar_items;
        for (auto& item : items) item = scale.Rect(item);
        out.playlist_.toolbar_items = items;
    }

    out.lyric_ = lyric_;
    out.lyric_.position = scale.Rect(lyric_.position);
    out.lyric_.lyric_bounds = scale.Rect(lyric_.lyric_bounds);
    out.lyric_.mini_padding = scale.Rect(lyric_.mini_padding);
    out.lyric_.title = ScaleElement(lyric_.title, scale);
    out.lyric_.close = ScaleElement(lyric_.close, scale);
    out.lyric_.ontop = ScaleElement(lyric_.ontop, scale);
    out.lyric_.desklrc = ScaleElement(lyric_.desklrc, scale);
    ScaleLogfont(out.lyric_.font, scale);
    ScaleLogfont(out.lyric_.mini_font, scale);

    out.desktop_lyric_bar_ = desktop_lyric_bar_;
    out.desktop_lyric_bar_.icon = scale.Rect(desktop_lyric_bar_.icon);
    out.desktop_lyric_bar_.play = ScaleElement(desktop_lyric_bar_.play, scale);
    out.desktop_lyric_bar_.pause = ScaleElement(desktop_lyric_bar_.pause, scale);
    out.desktop_lyric_bar_.previous = ScaleElement(desktop_lyric_bar_.previous, scale);
    out.desktop_lyric_bar_.next = ScaleElement(desktop_lyric_bar_.next, scale);
    out.desktop_lyric_bar_.list = ScaleElement(desktop_lyric_bar_.list, scale);
    out.desktop_lyric_bar_.settings = ScaleElement(desktop_lyric_bar_.settings, scale);
    out.desktop_lyric_bar_.karaoke = ScaleElement(desktop_lyric_bar_.karaoke, scale);
    out.desktop_lyric_bar_.lines = ScaleElement(desktop_lyric_bar_.lines, scale);
    out.desktop_lyric_bar_.lock = ScaleElement(desktop_lyric_bar_.lock, scale);
    out.desktop_lyric_bar_.ontop = ScaleElement(desktop_lyric_bar_.ontop, scale);
    out.desktop_lyric_bar_.zoom_in = ScaleElement(desktop_lyric_bar_.zoom_in, scale);
    out.desktop_lyric_bar_.zoom_out = ScaleElement(desktop_lyric_bar_.zoom_out, scale);
    out.desktop_lyric_bar_.return_to_window =
        ScaleElement(desktop_lyric_bar_.return_to_window, scale);
    out.desktop_lyric_bar_.close = ScaleElement(desktop_lyric_bar_.close, scale);

    out.browser_ = browser_;
    out.browser_.position = scale.Rect(browser_.position);
    out.browser_.content_bounds = scale.Rect(browser_.content_bounds);
    out.browser_.close = ScaleElement(browser_.close, scale);
    out.browser_.backward = ScaleElement(browser_.backward, scale);
    out.browser_.forward = ScaleElement(browser_.forward, scale);
    out.browser_.refresh = ScaleElement(browser_.refresh, scale);
    out.browser_.check = ScaleElement(browser_.check, scale);
    out.browser_.link = ScaleElement(browser_.link, scale);

    out.equalizer_ = equalizer_;
    out.equalizer_.position = scale.Rect(equalizer_.position);
    out.equalizer_.title = ScaleElement(equalizer_.title, scale);
    out.equalizer_.close = ScaleElement(equalizer_.close, scale);
    out.equalizer_.enabled = ScaleElement(equalizer_.enabled, scale);
    out.equalizer_.profile = ScaleElement(equalizer_.profile, scale);
    out.equalizer_.reset = ScaleElement(equalizer_.reset, scale);
    out.equalizer_.balance = ScaleElement(equalizer_.balance, scale);
    out.equalizer_.surround = ScaleElement(equalizer_.surround, scale);
    out.equalizer_.preamp = ScaleElement(equalizer_.preamp, scale);
    for (size_t i = 0; i < out.equalizer_.bands.size(); ++i)
        out.equalizer_.bands[i] = ScaleElement(equalizer_.bands[i], scale);

    out.visual_ = visual_;
    if (out.visual_.font) ScaleLogfont(*out.visual_.font, scale);

    // resize_rect / 位图源尺寸 / 字体字号字段（font_size 已是像素）保持逻辑，
    // 由栅格化函数内部 ×N 与 ×scale 分别处理（docs/HIGH_DPI_SKIN_SPEC.md §5.2）。
    return out;
}

HRGN LegacySkin::CreateWindowRegion(bool mini) const {
    return CreateWindowRegion(mini, ui::DpiScale{96});
}

HRGN LegacySkin::CreateWindowRegion(bool mini, const ui::DpiScale& scale) const {
    // 主窗/迷你窗背景是固定尺寸 SkinImage（非九宫格）。包成空 resize_rect 的
    // SkinBitmap，复用 DrawFrameBackground 的整幅缩放退化分支按物理尺寸重绘，
    // 再扫描颜色键/alpha 生成区域（docs/HIGH_DPI_SKIN_SPEC.md §6.11）。
    const auto& image = mini ? mini_.background : background_;
    const auto mask = image.IsGdiPlus() ? image.CoverageMask() : image;
    if (!mask) return nullptr;
    const COLORREF transparent = image.IsGdiPlus() ? RGB(0, 0, 0) : transparent_color_;
    SkinBitmap frame{mask, mask.Size()};
    // window_size_ is already in physical pixels: the window always consumes
    // the AtDpi() mirror, never the 96-DPI source object. Scaling it again
    // here would double-apply the factor.
    const SIZE physical = mini ? mini_.window_size : window_size_;
    return CreateFrameRegion(frame, RECT{}, physical.cx, physical.cy, false,
                             transparent, scale);
}

} // namespace ttplayer::skin
