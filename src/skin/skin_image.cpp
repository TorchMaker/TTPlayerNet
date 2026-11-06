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

// 解码后的 32bpp BGRA 像素缓存。皮肤 PNG 每次启动都要走 GDI+ 解码 +
// LockBits 逐行拷贝，图片越多越慢；缓存命中时直接读像素重建 DIB/Bitmap，
// 省掉 PNG 解码。缓存放在皮肤解压缓存目录下（目录名已含包指纹），换皮肤或
// 皮肤包改动会自动失效；源文件大小 + 时间戳做第二重校验。
constexpr char kCacheMagic[8] = {'T', 'T', 'S', 'K', 'I', 'N', '0', '2'};
// 直接写最终文件（不再 temp + remove + rename：实测 74 个文件后者 1836ms、
// 直接写 222ms，实时防护会把每次 rename 放大约 25ms）。用尾部标记识别半截文件。
constexpr char kCacheFooter[8] = {'T', 'T', 'S', 'K', 'E', 'N', 'D', '!'};
struct CacheHeader {
    char magic[8];
    uint32_t width;
    uint32_t height;
    uint64_t source_size;
    uint64_t source_stamp;
    uint32_t scale;
    uint32_t reserved;
};
static_assert(sizeof(CacheHeader) == 40, "cache header must be packed");

// 默认皮肤（以及绝大多数 5.7.9 皮肤）用的是 24bpp BI_RGB BMP，走
// LoadImageW(LR_CREATEDIBSECTION) 分支，上面那份 PNG/GDI+ 缓存完全不会写入。
// 这里缓存 DIB 原始行：保留 biHeight 正负（自下而上 / 自上而下）与 stride，
// 命中时用 CreateDIBSection 原样重建，色键 StretchBlt/TransparentBlt 路径不变。
// 调色板（<=8bpp）BMP 不走缓存，避免丢失颜色表。
constexpr char kBmpCacheMagic[8] = {'T', 'T', 'S', 'K', 'B', 'M', '0', '2'};
#pragma pack(push, 1)
struct BmpCacheHeader {
    char magic[8];
    uint64_t source_size;
    uint64_t source_stamp;
    uint32_t width;
    uint32_t height;
    int32_t bi_height;
    uint32_t bit_count;
    uint32_t stride;
    uint32_t scale;
    uint32_t reserved;
};
#pragma pack(pop)
static_assert(sizeof(BmpCacheHeader) == 52, "bmp cache header must be packed");

bool DecodedCacheEnabled() {
    static const bool enabled = [] {
        wchar_t value[8]{};
        const DWORD length = GetEnvironmentVariableW(
            L"TTPLAYER_SKIN_CACHE", value, 8);
        // 默认关闭。实测默认皮肤（55 张 24bpp BMP）命中只省 ~2ms，但首次
        // 生成 74 个缓存文件要 1.2s，净效果是拖慢冷启动。PNG 皮肤较多时可
        // 用 TTPLAYER_SKIN_CACHE=1 显式开启（PNG 解码比 LoadImageW 贵得多）。
        return length == 1 && value[0] == L'1';
    }();
    return enabled;
}
}

struct SkinImage::Storage {
    std::shared_ptr<GraphicsRuntime> runtime;
    IStream* stream{};
    std::unique_ptr<Gdiplus::Bitmap> image;
    HBITMAP dib{};
    SIZE size{};
    ~Storage() {
        image.reset(); // GDI+ may lazily read its stream until destruction.
        if (stream) stream->Release();
        if (dib) DeleteObject(dib);
    }
};

SkinImage SkinImage::Load(const std::filesystem::path& path) {
    SkinImage result;
    int multiplier = 1;
    const std::filesystem::path chosen = PickHiResPath(path, &multiplier);
    const bool cache_enabled = DecodedCacheEnabled();
    const auto cache_file = chosen.parent_path() / L".ttp-images" /
        (chosen.filename().wstring() + L".bin");
    uint64_t source_size = 0;
    uint64_t source_stamp = 0;
    if (cache_enabled) {
        std::error_code meta;
        source_size = std::filesystem::file_size(chosen, meta);
        if (!meta)
            source_stamp = static_cast<uint64_t>(
                std::filesystem::last_write_time(chosen, meta)
                    .time_since_epoch().count());
        if (meta) { source_size = 0; source_stamp = 0; }
    }

    // 这两个 lambda 必须定义在成员函数内部，才能命名私有的 Storage 类型。
    auto try_cache = [&]() -> std::shared_ptr<Storage> {
        if (!source_size || !source_stamp) return {};
        std::ifstream input(cache_file, std::ios::binary);
        if (!input) return {};
        CacheHeader header{};
        if (!input.read(reinterpret_cast<char*>(&header), sizeof(header))) return {};
        if (std::memcmp(header.magic, kCacheMagic, sizeof(kCacheMagic)) != 0 ||
            header.source_size != source_size ||
            header.source_stamp != source_stamp ||
            header.scale != static_cast<uint32_t>(multiplier)) return {};
        if (!header.width || !header.height ||
            header.width > 16384 || header.height > 16384) return {};
        const uint64_t byte_count = static_cast<uint64_t>(header.width) *
                                    header.height * 4;
        if (!byte_count || byte_count > 16ull * 1024 * 1024) return {};
        std::vector<unsigned char> pixels(static_cast<size_t>(byte_count));
        if (!input.read(reinterpret_cast<char*>(pixels.data()),
                        static_cast<std::streamsize>(pixels.size()))) return {};
        char footer[sizeof(kCacheFooter)]{};
        if (!input.read(footer, sizeof(footer)) ||
            std::memcmp(footer, kCacheFooter, sizeof(footer)) != 0) return {};
        auto data = std::make_shared<Storage>();
        data->runtime = AcquireGraphics();
        if (!data->runtime->token) return {};
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = static_cast<LONG>(header.width);
        info.bmiHeader.biHeight = -static_cast<LONG>(header.height);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        void* dib_pixels{};
        data->dib = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS,
                                     &dib_pixels, nullptr, 0);
        if (!data->dib || !dib_pixels) return {};
        std::memcpy(dib_pixels, pixels.data(), pixels.size());
        // 与 CoverageMask() 相同的构造方式：GDI+ Bitmap 直接引用 DIB 像素，
        // 不额外分配、不需要 stream；Storage 析构时先释放 image 再删 DIB。
        data->image = std::make_unique<Gdiplus::Bitmap>(
            header.width, header.height, header.width * 4,
            PixelFormat32bppARGB, static_cast<BYTE*>(dib_pixels));
        if (!data->image || data->image->GetLastStatus() != Gdiplus::Ok) {
            data->image.reset();
            return {};
        }
        data->size = {static_cast<LONG>(header.width),
                      static_cast<LONG>(header.height)};
        return data;
    };

    auto save_cache = [&](const std::shared_ptr<Storage>& data) {
        if (!source_size || !source_stamp || !data || !data->image ||
            !data->dib) return;
        DIBSECTION section{};
        if (!GetObjectW(data->dib, sizeof(section), &section) ||
            !section.dsBm.bmBits || section.dsBm.bmBitsPixel != 32) return;
        const uint64_t byte_count = static_cast<uint64_t>(data->size.cx) *
                                    data->size.cy * 4;
        if (!byte_count || byte_count > 16ull * 1024 * 1024) return;
        std::error_code error;
        std::filesystem::create_directories(cache_file.parent_path(), error);
        if (error) return;
        {
            std::ofstream output(cache_file, std::ios::binary | std::ios::trunc);
            if (!output) return;
            CacheHeader header{};
            std::memcpy(header.magic, kCacheMagic, sizeof(kCacheMagic));
            header.width = static_cast<uint32_t>(data->size.cx);
            header.height = static_cast<uint32_t>(data->size.cy);
            header.source_size = source_size;
            header.source_stamp = source_stamp;
            header.scale = static_cast<uint32_t>(multiplier);
            output.write(reinterpret_cast<const char*>(&header), sizeof(header));
            const auto* source = static_cast<const unsigned char*>(section.dsBm.bmBits);
            for (LONG row = 0; row < data->size.cy; ++row)
                output.write(reinterpret_cast<const char*>(
                    source + static_cast<ptrdiff_t>(row) * section.dsBm.bmWidthBytes),
                    static_cast<std::streamsize>(data->size.cx) * 4);
            output.write(kCacheFooter, sizeof(kCacheFooter));
        }
    };

    auto try_bmp_cache = [&]() -> std::shared_ptr<Storage> {
        if (!source_size || !source_stamp) return {};
        std::ifstream input(cache_file, std::ios::binary);
        if (!input) return {};
        BmpCacheHeader header{};
        if (!input.read(reinterpret_cast<char*>(&header), sizeof(header))) return {};
        if (std::memcmp(header.magic, kBmpCacheMagic, sizeof(kBmpCacheMagic)) != 0 ||
            header.source_size != source_size ||
            header.source_stamp != source_stamp ||
            header.scale != static_cast<uint32_t>(multiplier)) return {};
        if (header.bit_count != 24 && header.bit_count != 32) return {};
        if (!header.width || !header.height ||
            header.width > 16384 || header.height > 16384) return {};
        const uint64_t expected_stride =
            (static_cast<uint64_t>(header.width) * header.bit_count + 31) / 32 * 4;
        if (header.stride != expected_stride) return {};
        const uint64_t byte_count = expected_stride * header.height;
        if (!byte_count || byte_count > 16ull * 1024 * 1024) return {};
        if (header.bi_height != static_cast<int32_t>(header.height) &&
            header.bi_height != -static_cast<int32_t>(header.height)) return {};
        std::vector<unsigned char> pixels(static_cast<size_t>(byte_count));
        if (!input.read(reinterpret_cast<char*>(pixels.data()),
                        static_cast<std::streamsize>(pixels.size()))) return {};
        char footer[sizeof(kCacheFooter)]{};
        if (!input.read(footer, sizeof(footer)) ||
            std::memcmp(footer, kCacheFooter, sizeof(footer)) != 0) return {};
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = static_cast<LONG>(header.width);
        info.bmiHeader.biHeight = header.bi_height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = static_cast<WORD>(header.bit_count);
        info.bmiHeader.biCompression = BI_RGB;
        void* target{};
        HBITMAP dib = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS,
                                       &target, nullptr, 0);
        if (!dib || !target) {
            if (dib) DeleteObject(dib);
            return {};
        }
        std::memcpy(target, pixels.data(), pixels.size());
        auto data = std::make_shared<Storage>();
        data->dib = dib;
        data->size = {static_cast<LONG>(header.width),
                      static_cast<LONG>(header.height)};
        return data;
    };

    auto save_bmp_cache = [&](const std::shared_ptr<Storage>& data) {
        if (!source_size || !source_stamp || !data || !data->dib) return;
        DIBSECTION section{};
        if (GetObjectW(data->dib, sizeof(section), &section) != sizeof(DIBSECTION) ||
            !section.dsBm.bmBits || section.dsBmih.biCompression != BI_RGB) return;
        const uint32_t bit_count = section.dsBm.bmBitsPixel;
        if (bit_count != 24 && bit_count != 32) return;
        const LONG width = section.dsBm.bmWidth;
        const LONG height = section.dsBm.bmHeight;
        if (width <= 0 || height <= 0) return;
        const uint64_t stride =
            (static_cast<uint64_t>(width) * bit_count + 31) / 32 * 4;
        if (static_cast<uint64_t>(section.dsBm.bmWidthBytes) != stride) return;
        const uint64_t byte_count = stride * static_cast<uint64_t>(height);
        if (!byte_count || byte_count > 16ull * 1024 * 1024) return;
        if (section.dsBmih.biHeight != height &&
            section.dsBmih.biHeight != -height) return;
        BmpCacheHeader header{};
        std::memcpy(header.magic, kBmpCacheMagic, sizeof(kBmpCacheMagic));
        header.source_size = source_size;
        header.source_stamp = source_stamp;
        header.width = static_cast<uint32_t>(width);
        header.height = static_cast<uint32_t>(height);
        header.bi_height = section.dsBmih.biHeight;
        header.bit_count = bit_count;
        header.stride = static_cast<uint32_t>(stride);
        header.scale = static_cast<uint32_t>(multiplier);
        std::error_code error;
        std::filesystem::create_directories(cache_file.parent_path(), error);
        if (error) return;
        {
            std::ofstream output(cache_file, std::ios::binary | std::ios::trunc);
            if (!output) return;
            output.write(reinterpret_cast<const char*>(&header), sizeof(header));
            const auto* source = static_cast<const unsigned char*>(section.dsBm.bmBits);
            for (LONG row = 0; row < height; ++row)
                output.write(reinterpret_cast<const char*>(
                    source + static_cast<ptrdiff_t>(row) * section.dsBm.bmWidthBytes),
                    static_cast<std::streamsize>(stride));
            output.write(kCacheFooter, sizeof(kCacheFooter));
        }
    };

    if (auto cached = try_cache()) {
        result.storage_ = std::move(cached);
        result.source_scale_ = multiplier;
        return result;
    }
    if (auto cached = try_bmp_cache()) {
        result.storage_ = std::move(cached);
        result.source_scale_ = multiplier;
        return result;
    }

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
    if (bytes[0] == 'B' && bytes[1] == 'M' && bmp_length == bytes.size()) {
        // Preserve the 5.7.9 DIB + colour-key path, including its palette.
        data->dib = static_cast<HBITMAP>(LoadImageW(nullptr, path.c_str(),
            IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE | LR_CREATEDIBSECTION));
        if (!data->dib) return result;
        BITMAP bitmap{};
        GetObjectW(data->dib, sizeof(bitmap), &bitmap);
        data->size = {bitmap.bmWidth, bitmap.bmHeight};
        // LoadImageW 用的是原始 path；只有未命中 @Nx 替换时缓存元数据才对得上。
        if (chosen == path) save_bmp_cache(data);
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
        save_cache(data);
    }
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
