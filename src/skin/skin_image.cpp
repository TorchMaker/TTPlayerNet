#include "ttplayer/skin/skin_image.h"

#include "ttplayer/platform/optional_windows_api.h"
#include "ttplayer/ui/dpi_scale.h"

#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <system_error>
#include <vector>

namespace ttplayer::skin {
namespace {

// Choose an @Nx sibling when present, nearest the system effective DPI. An @Nx
// asset's source pixels equal logical * N, so nine-grid split and frame offsets
// stay correct without touching the XML (docs/HIGH_DPI_SKIN_SPEC.md §7). Returns
// the chosen path and writes *multiplier (1, 2 or 3).
std::filesystem::path PickHiResPath(const std::filesystem::path& original, int* multiplier) {
    *multiplier = 1;
    // GetDpiForSystem is a plain function (lazily resolved import), not a
    // function pointer: call it and test the returned value. Prefer the
    // session DPI (the main window's monitor) over the system DPI.
    int system_dpi = ui::SessionDpi();
    if (system_dpi <= 0) system_dpi = 96;
    if (system_dpi <= 0) system_dpi = 96;
    const double factor = static_cast<double>(system_dpi) / 96.0;
    const int wanted = factor >= 2.25 ? 3 : (factor >= 1.5 ? 2 : 1);
    if (wanted <= 1) return original;
    auto with_suffix = [&](int n) {
        std::filesystem::path candidate = original;
        candidate.replace_extension(L"@" + std::to_wstring(n) + original.extension().wstring());
        return candidate;
    };
    std::error_code ec;
    if (wanted == 3) {
        const auto p3 = with_suffix(3);
        if (std::filesystem::exists(p3, ec)) { *multiplier = 3; return p3; }
    }
    const auto p2 = with_suffix(2);
    if (std::filesystem::exists(p2, ec)) { *multiplier = 2; return p2; }
    return original;
}
struct GraphicsRuntime {
    ULONG_PTR token{};
    GraphicsRuntime() {
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok)
            token = 0;
    }
    ~GraphicsRuntime() { if (token) Gdiplus::GdiplusShutdown(token); }
};
std::shared_ptr<GraphicsRuntime> AcquireGraphics() {
    // No shutdown while an image still exists (including copies across a
    // transactional skin rebind). Skin loading/drawing runs on the UI thread.
    static std::weak_ptr<GraphicsRuntime> cached;
    static std::mutex mutex;
    const std::lock_guard lock(mutex);
    auto runtime = cached.lock();
    if (!runtime) cached = runtime = std::make_shared<GraphicsRuntime>();
    return runtime;
}
}

struct SkinImage::Storage {
    std::shared_ptr<GraphicsRuntime> runtime;
    IStream* stream{};
    std::unique_ptr<Gdiplus::Bitmap> image;
    HBITMAP dib{};
    SIZE size{};
    struct RegionEntry { COLORREF key; int frames; HRGN region; COLORREF drawing_key; };
    std::vector<RegionEntry> regions;
    ~Storage() {
        for (const auto& entry : regions) DeleteObject(entry.region);
        image.reset(); // GDI+ may lazily read its stream until destruction.
        if (stream) stream->Release();
        if (dib) DeleteObject(dib);
    }
};

RECT NativeSkinIconBounds(HICON icon, RECT bounds) {
    ICONINFO info{};
    BITMAP bitmap{};
    if (icon && GetIconInfo(icon, &info)) {
        GetObjectW(info.hbmColor ? info.hbmColor : info.hbmMask, sizeof(bitmap), &bitmap);
        if (!info.hbmColor) bitmap.bmHeight /= 2;
        if (info.hbmColor) DeleteObject(info.hbmColor);
        if (info.hbmMask) DeleteObject(info.hbmMask);
    }
    bounds.right = bounds.left + bitmap.bmWidth;
    bounds.bottom = bounds.top + bitmap.bmHeight;
    return bounds;
}

void DrawNativeSkinIcon(HDC dc, HICON icon, const RECT& bounds) {
    ICONINFO info{};
    if (!icon || !GetIconInfo(icon, &info)) return;
    BITMAP bitmap{};
    GetObjectW(info.hbmColor ? info.hbmColor : info.hbmMask, sizeof(bitmap), &bitmap);
    if (!info.hbmColor) bitmap.bmHeight /= 2;
    if (info.hbmColor) DeleteObject(info.hbmColor);
    if (info.hbmMask) DeleteObject(info.hbmMask);
    const int saved = SaveDC(dc);
    if (!saved) return;
    IntersectClipRect(dc, bounds.left, bounds.top, bounds.right, bounds.bottom);
    DrawIconEx(dc, bounds.left + (bounds.right - bounds.left - bitmap.bmWidth) / 2,
        bounds.top + (bounds.bottom - bounds.top - bitmap.bmHeight) / 2,
        icon, bitmap.bmWidth, bitmap.bmHeight, 0, nullptr, DI_NORMAL);
    RestoreDC(dc, saved);
}

SkinImage SkinImage::Load(const std::filesystem::path& path, bool* replacement_started) {
    if (replacement_started) *replacement_started = false;
    SkinImage result;
    int multiplier = 1;
    const std::filesystem::path chosen = PickHiResPath(path, &multiplier);
    std::ifstream input(chosen, std::ios::binary | std::ios::ate);
    if (!input) return result;
    const auto length = input.tellg();
    // Bound allocation and decoded dimensions for malformed skin packages.
    if (length < 6 || length > 64 * 1024 * 1024) return result;
    std::vector<unsigned char> bytes(static_cast<size_t>(length));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(bytes.data()), length)) return result;
    auto data = std::make_shared<Storage>();
    DWORD bmp_length{};
    std::memcpy(&bmp_length, bytes.data() + 2, sizeof(bmp_length));
    if (bytes[0] == 'B' && bytes[1] == 'M') {
        if (bmp_length != bytes.size() || bytes.size() < 14) return result;
        if (replacement_started) *replacement_started = true;
        if (bytes.size() < 54) return result;
        BITMAPINFOHEADER header{};
        std::memcpy(&header, bytes.data() + 14, sizeof(header));
        DWORD offset{};
        std::memcpy(&offset, bytes.data() + 10, sizeof(offset));
        // 00408CF4 accepts a BITMAPINFOHEADER/BI_RGB DIB. Validate lengths
        // before asking GDI to decode; never restore the original overreads.
        if (header.biSize != sizeof(header) || header.biCompression != BI_RGB ||
            header.biPlanes != 1 || header.biWidth <= 0 || header.biWidth > 16384 ||
            !header.biHeight || header.biHeight < -16384 || header.biHeight > 16384 ||
            (header.biBitCount != 1 && header.biBitCount != 4 && header.biBitCount != 8 &&
             header.biBitCount != 16 && header.biBitCount != 24 && header.biBitCount != 32)) return result;
        const uint64_t height = std::abs(header.biHeight);
        const uint64_t stride = ((static_cast<uint64_t>(header.biWidth) * header.biBitCount + 31) / 32) * 4;
        const uint64_t colors = header.biClrUsed ? header.biClrUsed :
            (header.biBitCount <= 8 ? 1ULL << header.biBitCount : 0);
        if (static_cast<uint64_t>(header.biWidth) * height > 32 * 1024 * 1024 ||
            (header.biBitCount <= 8 && colors > (1ULL << header.biBitCount)) ||
            offset < 54 + colors * 4 || offset > bytes.size() || stride * height > bytes.size() - offset)
            return result;
        // 00408CF4 copies the original DIB pixels. LoadImage may remap equal
        // palette colours to one index, changing the 8-bit button hit region.
        void* pixels{};
        data->dib = CreateDIBSection(nullptr,
            reinterpret_cast<const BITMAPINFO*>(bytes.data() + 14),
            DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!data->dib || !pixels) return result;
        std::memcpy(pixels, bytes.data() + offset, static_cast<size_t>(stride * height));
        data->size = {header.biWidth, static_cast<LONG>(height)};
    } else {
        // 00522A01 -> 0046A76A -> GdipCreateBitmapFromStream. Dispatch by
        // bytes, not filename extension; RGB PNG also belongs to this path.
        data->runtime = AcquireGraphics();
        if (!data->runtime->token) return result;
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
        if (!memory) return result;
        void* target = GlobalLock(memory);
        if (!target) { GlobalFree(memory); return result; }
        std::memcpy(target, bytes.data(), bytes.size());
        GlobalUnlock(memory);
        if (FAILED(CreateStreamOnHGlobal(memory, TRUE, &data->stream))) {
            GlobalFree(memory);
            return result;
        }
        data->image.reset(Gdiplus::Bitmap::FromStream(data->stream, FALSE));
        if (!data->image || data->image->GetLastStatus() != Gdiplus::Ok) return result;
        const UINT width = data->image->GetWidth(), height = data->image->GetHeight();
        if (!width || !height || width > 16384 || height > 16384 ||
            static_cast<ULONGLONG>(width) * height > 32 * 1024 * 1024) return result;
        data->size = {static_cast<LONG>(width), static_cast<LONG>(height)};
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -static_cast<LONG>(height);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        void* pixels{};
        data->dib = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!data->dib) return result;
        Gdiplus::BitmapData locked{};
        Gdiplus::Rect rect(0, 0, width, height);
        if (data->image->LockBits(&rect, Gdiplus::ImageLockModeRead,
                PixelFormat32bppARGB, &locked) != Gdiplus::Ok) return result;
        for (UINT row = 0; row < height; ++row)
            std::memcpy(static_cast<unsigned char*>(pixels) + row * width * 4,
                static_cast<unsigned char*>(locked.Scan0) +
                    static_cast<ptrdiff_t>(row) * locked.Stride, width * 4);
        data->image->UnlockBits(&locked);
    }
    if (replacement_started) *replacement_started = true;
    result.storage_ = std::move(data);
    result.source_scale_ = multiplier;
    return result;
}

SkinImage::operator HBITMAP() const noexcept {
    return storage_ ? storage_->dib : borrowed_;
}
bool SkinImage::IsGdiPlus() const noexcept { return storage_ && storage_->image; }
SIZE SkinImage::Size() const noexcept {
    if (storage_) return storage_->size;
    BITMAP bitmap{};
    if (borrowed_) GetObjectW(borrowed_, sizeof(bitmap), &bitmap);
    return {bitmap.bmWidth, bitmap.bmHeight};
}

SkinImage SkinImage::CoverageMask() const {
    SkinImage result;
    if (!IsGdiPlus()) return result;
    // The PNG compatibility DIB contains straight BGRA copied by LockBits.
    // GetDIBits may discard its alpha, so read the owned DIB pixels directly.
    DIBSECTION source{};
    if (!GetObjectW(storage_->dib, sizeof(source), &source) ||
        !source.dsBm.bmBits || source.dsBm.bmBitsPixel != 32) return result;
    auto data = std::make_shared<Storage>();
    data->size = storage_->size;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = data->size.cx;
    info.bmiHeader.biHeight = -data->size.cy;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* pixels{};
    data->dib = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!data->dib || !pixels) return result;
    auto* target = static_cast<DWORD*>(pixels);
    const auto* bytes = static_cast<const unsigned char*>(source.dsBm.bmBits);
    for (LONG y = 0; y < data->size.cy; ++y) {
        const auto* row = bytes + static_cast<size_t>(y) * source.dsBm.bmWidthBytes;
        for (LONG x = 0; x < data->size.cx; ++x)
            *target++ = row[x * 4 + 3] ? 0xffffffff : 0xff000000;
    }
    // A resized PNG mask must use the same nearest/half-pixel sampling as
    // the image, not StretchBlt's device-dependent downsampling mode.
    data->runtime = storage_->runtime;
    data->image = std::make_unique<Gdiplus::Bitmap>(data->size.cx, data->size.cy,
        data->size.cx * 4, PixelFormat32bppARGB, static_cast<BYTE*>(pixels));
    if (data->image->GetLastStatus() != Gdiplus::Ok) return result;
    result.storage_ = std::move(data);
    return result;
}

SkinImage SkinImage::DarkenedBufferFill(COLORREF key) const {
    // 0045269C only derives fill_image2 from a 24-bit DIB. Keep mask pixels
    // intact; halving them would turn transparent pink into a visible stripe.
    BITMAP source{};
    if (IsGdiPlus() || !GetObjectW(*this, sizeof(source), &source) ||
        source.bmBitsPixel != 24) return {};
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = source.bmWidth;
    info.bmiHeader.biHeight = -source.bmHeight;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 24;
    auto data = std::make_shared<Storage>();
    data->size = {source.bmWidth, source.bmHeight};
    BYTE* pixels{};
    const HDC dc = CreateCompatibleDC(nullptr);
    data->dib = CreateDIBSection(dc, &info, DIB_RGB_COLORS,
        reinterpret_cast<void**>(&pixels), nullptr, 0);
    const int rows = data->dib && pixels ? GetDIBits(dc, *this, 0, source.bmHeight,
        pixels, &info, DIB_RGB_COLORS) : 0;
    DeleteDC(dc);
    if (rows != source.bmHeight) return {};
    const int stride = (source.bmWidth * 3 + 3) & ~3;
    for (int y = 0; y < source.bmHeight; ++y) for (int x = 0; x < source.bmWidth; ++x) {
        BYTE* pixel = pixels + y * stride + x * 3;
        if (key != CLR_INVALID && RGB(pixel[2], pixel[1], pixel[0]) == key) continue;
        pixel[0] /= 2; pixel[1] /= 2; pixel[2] /= 2;
    }
    SkinImage result;
    result.storage_ = std::move(data);
    return result;
}

namespace {
bool IndexedButtonRuns(HBITMAP bitmap, COLORREF key, std::vector<RECT>& runs, bool& disable_key) {
    DIBSECTION section{};
    if (!GetObjectW(bitmap, sizeof(section), &section) || !section.dsBm.bmBits ||
        section.dsBm.bmBitsPixel != 8) return false;
    const HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;
    const auto old = SelectObject(dc, bitmap);
    RGBQUAD colors[256]{};
    const UINT count = GetDIBColorTable(dc, 0, 256, colors);
    SelectObject(dc, old);
    DeleteDC(dc);
    if (!count) return false;
    UINT transparent = count;
    for (UINT i = 0; i < count; ++i)
        if (RGB(colors[i].rgbRed, colors[i].rgbGreen, colors[i].rgbBlue) == key) {
            transparent = i; break;
        }
    disable_key = transparent == count;
    if (disable_key) return true;
    const int width = section.dsBm.bmWidth / 4, height = section.dsBm.bmHeight;
    for (int y = 0; y < height; ++y) {
        const int row = section.dsBmih.biHeight < 0 ? y : height - 1 - y;
        const auto* pixels = static_cast<const BYTE*>(section.dsBm.bmBits) + row * section.dsBm.bmWidthBytes;
        int start = -1;
        for (int x = 0; x <= width; ++x) {
            const bool opaque = x < width && pixels[x] != transparent;
            if (opaque && start < 0) start = x;
            if (!opaque && start >= 0) { runs.push_back({start,y,x,y+1}); start = -1; }
        }
    }
    // 00452FFF: no runs from the 8-bit path means a rectangular button and
    // CLR_INVALID for every state, not an invisible/click-through window.
    disable_key = runs.empty();
    return true;
}
}

COLORREF SkinImage::ButtonColorKey(COLORREF key) const {
    if (IsGdiPlus()) return key;
    const HRGN region = CreateRegion(key, 4);
    if (region) DeleteObject(region);
    if (storage_) for (const auto& entry : storage_->regions)
        if (entry.key == key && entry.frames == 4) return entry.drawing_key;
    std::vector<RECT> runs;
    bool disable{};
    return IndexedButtonRuns(*this, key, runs, disable) && disable ? CLR_INVALID : key;
}

HRGN SkinImage::CreateRegion(COLORREF key, int frames) const {
    const SIZE size = Size();
    const int width = size.cx / std::max(1, frames);
    if (!static_cast<HBITMAP>(*this) || width <= 0 || size.cy <= 0) return nullptr;
    const auto copy = [](HRGN source) {
        if (!source) return static_cast<HRGN>(nullptr);
        const HRGN result = CreateRectRgn(0, 0, 0, 0);
        CombineRgn(result, source, nullptr, RGN_COPY);
        return result;
    };
    if (storage_) for (const auto& entry : storage_->regions)
        if (entry.key == key && entry.frames == frames) return copy(entry.region);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -size.cy;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    DWORD* pixels{};
    const HDC dc = CreateCompatibleDC(nullptr);
    const HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS,
        reinterpret_cast<void**>(&pixels), nullptr, 0);
    if (!dc || !bitmap || !pixels) {
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        return nullptr;
    }
    const auto old = SelectObject(dc, bitmap);
    const bool alpha = IsGdiPlus();
    if (alpha) CoverageMask().Draw(dc, 0, 0, width, size.cy, 0, 0, width, size.cy);
    else Draw(dc, 0, 0, width, size.cy, 0, 0, width, size.cy);
    GdiFlush();
    const DWORD rgb = (GetRValue(key) << 16) | (GetGValue(key) << 8) | GetBValue(key);
    std::vector<RECT> runs;
    bool disable_key{};
    const bool indexed = frames == 4 && !alpha && IndexedButtonRuns(*this, key, runs, disable_key);
    if (indexed && disable_key) runs.push_back({0, 0, width, size.cy});
    for (int y = 0; !indexed && y < size.cy; ++y) {
        int start = -1;
        for (int x = 0; x <= width; ++x) {
            const bool opaque = x < width && (alpha ? (pixels[y * width + x] & 0xffffff) != 0
                : key == CLR_INVALID || (pixels[y * width + x] & 0xffffff) != rgb);
            if (opaque && start < 0) start = x;
            if (!opaque && start >= 0) { runs.push_back({start,y,x,y+1}); start = -1; }
        }
    }
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    HRGN region{};
    if (!runs.empty()) {
        const size_t bytes = sizeof(RGNDATAHEADER) + runs.size() * sizeof(RECT);
        std::vector<unsigned char> data(bytes);
        auto* rgn = reinterpret_cast<RGNDATA*>(data.data());
        rgn->rdh = {sizeof(RGNDATAHEADER), RDH_RECTANGLES, static_cast<DWORD>(runs.size()),
            static_cast<DWORD>(runs.size() * sizeof(RECT)), {0,0,width,size.cy}};
        std::memcpy(rgn->Buffer, runs.data(), runs.size() * sizeof(RECT));
        region = ExtCreateRegion(nullptr, static_cast<DWORD>(bytes), rgn);
    }
    // 004506AF returns NULL when a non-indexed BMP has no opaque runs:
    // SetWindowRgn(NULL) leaves a rectangular HWND (and keeps the key).
    // PNG is an extension: an alpha-empty sprite has a genuinely empty hit
    // region. Do not collapse that region into the BMP rectangular fallback.
    if (!region && alpha) region = CreateRectRgn(0,0,0,0);
    if (storage_) {
        storage_->regions.push_back({key, frames, region, disable_key ? CLR_INVALID : key});
        return copy(region);
    }
    return region;
}

bool SkinImage::Draw(HDC dc, int x, int y, int width, int height,
    int sx, int sy, int sw, int sh, COLORREF transparent, BYTE opacity) const {
    if (!dc || !static_cast<HBITMAP>(*this) || width <= 0 || height <= 0 ||
        sw <= 0 || sh <= 0) return false;
    if (opacity == 0) return true;
    if (IsGdiPlus()) {
        // 0046A938/0046A9F6: native pixels, nearest-neighbour + half-pixel
        // offset, source-over alpha; the BMP magenta key does NOT apply.
        // @Nx assets (source_scale_ > 1) instead use bicubic so the extra
        // resolution survives downscaling to the on-screen DPI. The layout
        // multiplier (ui::DpiScale) is decoupled from the asset multiplier
        // (source_scale_) — see docs/HIGH_DPI_SKIN_SPEC.md §7.
        Gdiplus::Graphics graphics(dc);
        graphics.SetInterpolationMode(source_scale_ > 1
            ? Gdiplus::InterpolationModeHighQualityBicubic
            : Gdiplus::InterpolationModeNearestNeighbor);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        Gdiplus::ImageAttributes attributes;
        Gdiplus::ColorMatrix matrix{};
        for (int i = 0; i < 5; ++i) matrix.m[i][i] = 1.0f;
        matrix.m[3][3] = opacity / 255.0f;
        if (opacity != 255) attributes.SetColorMatrix(&matrix);
        return graphics.DrawImage(storage_->image.get(), Gdiplus::Rect(x,y,width,height),
            sx,sy,sw,sh,Gdiplus::UnitPixel, opacity == 255 ? nullptr : &attributes)
            == Gdiplus::Ok;
    }
    const HDC source = CreateCompatibleDC(dc);
    if (!source) return false;
    const HGDIOBJ old = SelectObject(source, static_cast<HBITMAP>(*this));
    const BOOL drawn = transparent == CLR_INVALID
        ? StretchBlt(dc,x,y,width,height,source,sx,sy,sw,sh,SRCCOPY)
        : TransparentBlt(dc,x,y,width,height,source,sx,sy,sw,sh,transparent);
    SelectObject(source, old);
    DeleteDC(source);
    return drawn != FALSE;
}

void SkinHoverAnimation::SetState(int next, const SkinAnimation& def, ULONGLONG now) {
    if (state == next) return;
    if (def.Enabled() && ((state == 0 && next == 1) || (state == 1 && next == 0))) {
        remaining = remaining > 0 ? def.frame_count - remaining : def.frame_count;
        last_tick = now;
    } else remaining = 0;
    state = next;
}
bool SkinHoverAnimation::Tick(const SkinAnimation& def, ULONGLONG now) {
    if (remaining <= 0 || !def.Enabled() ||
        now - last_tick < static_cast<ULONGLONG>(def.frame_interval)) return false;
    --remaining;
    last_tick = now;
    return true;
}
BYTE SkinHoverAnimation::HotOpacity(const SkinAnimation& def) const {
    if (remaining <= 0 || !def.Enabled()) return state == 1 ? 255 : 0;
    const int alpha = static_cast<int>(static_cast<int64_t>(remaining) * 255 / def.frame_count);
    return static_cast<BYTE>(std::clamp(state == 1 ? 255 - alpha : alpha, 0, 255));
}

void SkinPulseAnimation::SetPlaying(bool value, const SkinAnimation& def, ULONGLONG now) {
    if (value == playing) return;
    playing = value;
    if (value) {
        remaining = def.Enabled() ? def.frame_count : 0; // 004A9ED7
        last_tick = now;
    } else if (remaining > def.frame_count / 2) {
        remaining = def.frame_count - remaining; // 0042FFDB stop/reverse
    }
}
bool SkinPulseAnimation::Tick(const SkinAnimation& def, ULONGLONG now) {
    if (!def.Enabled() || (!playing && remaining <= 0) ||
        now - last_tick < static_cast<ULONGLONG>(def.frame_interval)) return false;
    if (playing && remaining <= 0) remaining = def.frame_count;
    if (remaining > 0) --remaining;
    last_tick = now;
    return true;
}
BYTE SkinPulseAnimation::Opacity(const SkinAnimation& def) const {
    if (!def.Enabled()) return 0;
    const int distance = std::min(remaining, def.frame_count - remaining);
    return static_cast<BYTE>(std::clamp<int64_t>(
        static_cast<int64_t>(distance) * 510 / def.frame_count, 0, 255));
}
} // namespace ttplayer::skin
