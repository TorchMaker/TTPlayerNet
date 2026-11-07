// MusicFree 兼容层 —— 单个插件封装实现
//
// 复刻 MusicFree plugin.ts 的：mountPlugin（读取 module.exports、抽取能力、
// 计算 hash）、PluginMethodsWrapper（search/getMediaSource/getLyric 的调用与
// 标准化）。调用插件方法后统一 await Promise。

#include "ttplayer/musicfree/musicfree_plugin.h"

#include <cstdint>
#include <sstream>
#include <stdexcept>

#include <quickjs.h>
#include <nlohmann/json.hpp>

namespace ttplayer::musicfree {

namespace {
// 内容指纹（用于去重/缓存键，非必须匹配 MusicFree 的 sha256）。
std::string ContentHash(std::string_view s) {
    std::uint64_t h = 1469598103934665603ULL; // FNV-1a 64
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    std::ostringstream os; os << std::hex << h; return os.str();
}

bool HasMethod(JSContext* ctx, JSValueConst instance, const char* name) {
    JSValue fn = JS_GetPropertyStr(ctx, instance, name);
    bool ok = JS_IsFunction(ctx, fn);
    JS_FreeValue(ctx, fn);
    return ok;
}
} // namespace

MusicFreePlugin::MusicFreePlugin(std::string source_url)
    : source_url_(std::move(source_url)) {
    engine_ = std::make_unique<QuickJsEngine>();
    engine_->SetLogSink([](std::string_view level, std::string_view msg) {
        // 默认：仅忽略（可改为 OutputDebugString / 文件日志）。
        (void)level; (void)msg;
    });
}

MusicFreePlugin::~MusicFreePlugin() {
    if (engine_ && !JS_IsUndefined(instance_)) JS_FreeValue(engine_->context(), instance_);
}

void MusicFreePlugin::Configure(NetworkConfig network, std::filesystem::path lib_dir) {
    engine_->SetNetworkConfig(std::move(network));
    engine_->SetLibDir(std::move(lib_dir));
}

bool MusicFreePlugin::Mount(std::string_view js_code, std::string_view app_version) {
    last_error_.clear();
    mounted_ = false;
    try {
        engine_->SetAppVersion(app_version);
        JSValue exports = engine_->EvalPlugin(js_code, source_url_);
        instance_ = exports; // 接管（engine_ 生命周期内有效）；出错路径在 catch 中释放
        JSContext* ctx = engine_->context();

        // 抽取元信息（复刻 mountPlugin 的 name/version/srcUrl/hash）
        JSValue platform = JS_GetPropertyStr(ctx, instance_, "platform");
        const char* p = JS_ToCString(ctx, platform);
        meta_.name = p ? p : "";
        if (p) JS_FreeCString(ctx, p);
        JS_FreeValue(ctx, platform);

        JSValue ver = JS_GetPropertyStr(ctx, instance_, "version");
        const char* v = JS_ToCString(ctx, ver);
        meta_.version = v ? v : "";
        if (v) JS_FreeCString(ctx, v);
        JS_FreeValue(ctx, ver);

        JSValue src = JS_GetPropertyStr(ctx, instance_, "srcUrl");
        const char* su = JS_ToCString(ctx, src);
        if (su) meta_.source_url = su;
        if (su) JS_FreeCString(ctx, su);
        JS_FreeValue(ctx, src);
        if (meta_.source_url.empty()) meta_.source_url = source_url_;

        meta_.hash = ContentHash(js_code);

        // 能力
        meta_.can_search = HasMethod(ctx, instance_, "search");
        meta_.can_media_source = HasMethod(ctx, instance_, "getMediaSource");
        meta_.can_lyric = HasMethod(ctx, instance_, "getLyric");
        meta_.can_music_info = HasMethod(ctx, instance_, "getMusicInfo");
        meta_.can_album = HasMethod(ctx, instance_, "getAlbumInfo");
        meta_.can_sheet = HasMethod(ctx, instance_, "getMusicSheetInfo");
        meta_.can_top_lists = HasMethod(ctx, instance_, "getTopLists");
        meta_.can_sheet_detail = HasMethod(ctx, instance_, "getTopListDetail") ||
                                HasMethod(ctx, instance_, "getMusicSheetInfo");
        meta_.can_recommend = HasMethod(ctx, instance_, "getRecommendSheetTags") &&
                              HasMethod(ctx, instance_, "getRecommendSheetsByTag");

        if (meta_.name.empty()) {
            last_error_ = "插件未提供 platform 名称";
            mounted_ = false;
            return false;
        }
        mounted_ = true;
        return true;
    } catch (const std::exception& e) {
        last_error_ = e.what();
        mounted_ = false;
        if (!JS_IsUndefined(instance_)) { JS_FreeValue(engine_->context(), instance_); instance_ = JS_UNDEFINED; }
        return false;
    }
}

JSValue MusicFreePlugin::CallMethod(std::string_view method, int argc, JSValueConst* argv) {
    engine_->RefreshStackTop();  // 后台线程调用插件：修正 QuickJS 栈溢出基准
    JSContext* ctx = engine_->context();
    JSValue fn = JS_GetPropertyStr(ctx, instance_, std::string(method).c_str());
    if (!JS_IsFunction(ctx, fn)) {
        JS_FreeValue(ctx, fn);
        throw std::runtime_error(std::string(method) + " 方法不存在");
    }
    JSValue promise = JS_Call(ctx, fn, instance_, argc, const_cast<JSValueConst*>(argv));
    JS_FreeValue(ctx, fn);
    if (JS_IsException(promise)) {
        // 取异常文本
        JSValue exc = JS_GetException(ctx);
        const char* c = JS_ToCString(ctx, exc);
        std::string text = c ? c : "未知错误";
        if (c) JS_FreeCString(ctx, c);
        JS_FreeValue(ctx, exc);
        // method 是 string_view，需先转成 std::string 才能与字面量拼接。
        throw std::runtime_error("插件 " + std::string(method) + " 调用异常: " + text);
    }
    JSValue result = engine_->Await(promise); // 内部已泵微任务
    JS_FreeValue(ctx, promise);
    return result;
}

SearchResult MusicFreePlugin::Search(std::string_view query, int page, std::string_view type) {
    SearchResult out;
    if (!mounted_ || !meta_.can_search) { out.error = "插件未挂载或不支持搜索"; return out; }
    JSContext* ctx = engine_->context();
    JSValue q = JS_NewString(ctx, std::string(query).c_str());
    JSValue pg = JS_NewInt32(ctx, page);
    JSValue t = JS_NewString(ctx, std::string(type).c_str());
    JSValue argv[3] = { q, pg, t };
    try {
        JSValue res = CallMethod("search", 3, argv);
        nlohmann::json j = QuickJsEngine::ValueToJson(ctx, res);
        JS_FreeValue(ctx, res);
        if (j.is_object()) {
            if (j.contains("isEnd") && j["isEnd"].is_boolean()) out.is_end = j["isEnd"].get<bool>();
            if (j.contains("data") && j["data"].is_array()) {
                for (auto& e : j["data"]) {
                    MusicItem item = MusicItem::FromJson(e);
                    if (item.platform.empty()) item.platform = meta_.name; // 复刻 resetMediaItem
                    if (item.raw.contains("platform") == false) item.raw["platform"] = meta_.name;
                    out.items.push_back(std::move(item));
                }
            }
        }
    } catch (const std::exception& e) {
        out.error = e.what();
    }
    JS_FreeValue(ctx, q); JS_FreeValue(ctx, pg); JS_FreeValue(ctx, t);
    return out;
}

std::optional<MediaSource> MusicFreePlugin::GetMediaSource(const MusicItem& item,
                                                           QualityKey quality,
                                                           std::string* error) {
    if (!mounted_ || !meta_.can_media_source) {
        if (error) *error = "音源未挂载或不导出 getMediaSource";
        return std::nullopt;
    }
    JSContext* ctx = engine_->context();
    JSValue music = QuickJsEngine::JsonToValue(ctx, item.raw);
    JSValue q = JS_NewString(ctx, std::string(ToString(quality)).c_str());
    JSValue argv[2] = { music, q };
    try {
        JSValue res = CallMethod("getMediaSource", 2, argv);
        nlohmann::json j = QuickJsEngine::ValueToJson(ctx, res);
        JS_FreeValue(ctx, res);
        JS_FreeValue(ctx, music); JS_FreeValue(ctx, q);
        if (j.is_object() && j.contains("url") && j["url"].is_string()) {
            MediaSource ms;
            ms.url = j["url"].get<std::string>();
            ms.quality = quality;
            if (j.contains("userAgent") && j["userAgent"].is_string())
                ms.user_agent = j["userAgent"].get<std::string>();
            if (j.contains("headers") && j["headers"].is_object())
                for (auto it = j["headers"].begin(); it != j["headers"].end(); ++it)
                    if (it.value().is_string()) ms.headers[it.key()] = it.value().get<std::string>();
            return ms;
        }
        if (error) {
            *error = j.is_object()
                ? "getMediaSource 没有返回 url 字段（音源可能只支持试听/需要登录）"
                : "getMediaSource 返回值不是对象";
        }
        return std::nullopt;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        JS_FreeValue(ctx, music); JS_FreeValue(ctx, q);
        return std::nullopt;
    }
}

std::optional<LyricResult> MusicFreePlugin::GetLyric(const MusicItem& item) {
    if (!mounted_ || !meta_.can_lyric) return std::nullopt;
    JSContext* ctx = engine_->context();
    JSValue music = QuickJsEngine::JsonToValue(ctx, item.raw);
    JSValue argv[1] = { music };
    try {
        JSValue res = CallMethod("getLyric", 1, argv);
        nlohmann::json j = QuickJsEngine::ValueToJson(ctx, res);
        JS_FreeValue(ctx, res);
        JS_FreeValue(ctx, music);
        if (j.is_object()) {
            LyricResult lr;
            auto pick = [&](const char* a, const char* b) -> std::string {
                if (j.contains(a) && j[a].is_string()) return j[a].get<std::string>();
                if (j.contains(b) && j[b].is_string()) return j[b].get<std::string>();
                return {};
            };
            lr.raw_lrc = pick("rawLrc", "lyric");
            lr.translation = pick("translation", "trans");
            if (lr.raw_lrc.empty() && lr.translation.empty()) return std::nullopt;
            return lr;
        }
        return std::nullopt;
    } catch (const std::exception&) {
        JS_FreeValue(ctx, music);
        return std::nullopt;
    }
}

// MusicItem::FromJson 的实现（定义在此，避免头文件膨胀）
MusicItem MusicItem::FromJson(const nlohmann::json& j) {
    MusicItem item;
    item.raw = j;
    // MusicFree 的 MediaItem 允许 id 等字段是 string 或 number；只认字符串会让
    // 「id」这种数字字段静默变成空串。网易云搜索返回的就是
    // "id":2716020167，空 id 传给 getMediaSource 后，音源拼出的解析地址是
    // `...?id=&level=...`，服务端只能返回错误，最终表现成「解析不出播放地址」。
    // 这里按 JSON 的原始数字文本转字符串（整数不会带上 .0），与 JS 的 String(n) 一致。
    auto get_str = [&](const char* k) -> std::string {
        if (!j.contains(k)) return {};
        const nlohmann::json& v = j[k];
        if (v.is_string()) return v.get<std::string>();
        if (v.is_number_integer() || v.is_number_unsigned()) return v.dump();
        if (v.is_number_float()) {
            // 个别路径仍可能把整数 id 传成浮点，这里去掉多余的 ".0"。
            const double d = v.get<double>();
            if (d >= -9223372036854775808.0 && d <= 9223372036854775807.0) {
                const long long i = static_cast<long long>(d);
                if (static_cast<double>(i) == d) return std::to_string(i);
            }
            return v.dump();
        }
        if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
        return {};
    };
    item.id = get_str("id");
    item.platform = get_str("platform");
    item.title = get_str("title");
    item.artist = get_str("artist");
    item.album = get_str("album");
    item.artwork = get_str("artwork");
    item.url = get_str("url");
    if (j.contains("duration") && j["duration"].is_number())
        item.duration_ms = static_cast<std::int64_t>(j["duration"].get<double>() * 1000.0);
    else if (j.contains("durationMs") && j["durationMs"].is_number())
        item.duration_ms = j["durationMs"].get<std::int64_t>();
    return item;
}

MusicSheet MusicSheet::FromJson(const nlohmann::json& j) {
    MusicSheet sheet;
    sheet.raw = j;
    auto get_str = [&](const char* k) -> std::string {
        if (!j.contains(k)) return {};
        const nlohmann::json& v = j[k];
        if (v.is_string()) return v.get<std::string>();
        if (v.is_number()) return v.dump();
        return {};
    };
    sheet.id = get_str("id");
    sheet.platform = get_str("platform");
    sheet.title = get_str("title");
    sheet.description = get_str("description");
    // 封面在 MusicFree 里叫 coverImg，部分插件用 artwork，两个都认。
    sheet.artwork = get_str("coverImg");
    if (sheet.artwork.empty()) sheet.artwork = get_str("artwork");
    sheet.work_id = get_str("workId");
    if (j.contains("playCount") && j["playCount"].is_number())
        sheet.play_count = j["playCount"].get<std::int64_t>();
    if (j.contains("createAt") && j["createAt"].is_number())
        sheet.create_at = j["createAt"].get<std::int64_t>();
    return sheet;
}

SheetTag SheetTag::FromJson(const nlohmann::json& j) {
    SheetTag tag;
    tag.raw = j;
    if (j.is_string()) {
        tag.id = j.get<std::string>();
        tag.title = tag.id;
    } else if (j.is_object()) {
        auto get_str = [&](const char* k) -> std::string {
            if (!j.contains(k) || !j[k].is_string()) return {};
            return j[k].get<std::string>();
        };
        tag.id = get_str("id");
        tag.title = get_str("title");
        if (tag.title.empty()) tag.title = tag.id;
    }
    return tag;
}

namespace {

// 把 getTopLists / getRecommendSheetsByTag 的返回（Array<IMusicSheetItem |
// IMusicSheetGroup>）解析成扁平的歌单列表。分组（group）里的每条记录都带上
// group 标题，UI 据此画出分区。
TopListsResult ParseSheetArray(const nlohmann::json& j) {
    TopListsResult out;
    if (!j.is_array()) return out;
    for (const auto& entry : j) {
        if (!entry.is_object()) continue;
        // 分组：{ title, data: [...] }
        if (entry.contains("data") && entry["data"].is_array()) {
            const std::string group = entry.value("title", std::string());
            for (const auto& item : entry["data"]) {
                MusicSheet sheet = MusicSheet::FromJson(item);
                sheet.group = group;
                out.items.push_back(std::move(sheet));
            }
        } else {
            out.items.push_back(MusicSheet::FromJson(entry));
        }
    }
    return out;
}

// 把 getTopListDetail / getMusicSheetInfo 的返回解析成曲目列表 + 是否末页。
// MusicFree 既可能返回 { musicList:[...], isEnd }，也可能返回 { data:[...] }，
// 或直接返回 IMusicItem[]；三种都兼容。
SheetTracksResult ParseSheetTracks(const nlohmann::json& j) {
    SheetTracksResult result;
    if (j.is_array()) {
        for (const auto& item : j) result.items.push_back(MusicItem::FromJson(item));
    } else if (j.is_object()) {
        const nlohmann::json* arr = nullptr;
        if (j.contains("musicList") && j["musicList"].is_array()) arr = &j["musicList"];
        else if (j.contains("data") && j["data"].is_array()) arr = &j["data"];
        else if (j.contains("list") && j["list"].is_array()) arr = &j["list"];
        if (arr) {
            for (const auto& item : *arr)
                result.items.push_back(MusicItem::FromJson(item));
        }
        if (j.contains("isEnd") && j["isEnd"].is_boolean())
            result.is_end = j["isEnd"].get<bool>();
        else if (j.contains("is_end") && j["is_end"].is_boolean())
            result.is_end = j["is_end"].get<bool>();
    }
    return result;
}

} // namespace

// 取详情类方法（getTopListDetail / getMusicSheetInfo / getRecommendSheetsByTag）
// 的结果：详情方法返回的是 Promise，这里统一 await 后转换为 C++ 结果。声明为
// MusicFreePlugin 的私有静态成员，以便访问私有的 CallMethod。
template <typename ResultT>
ResultT MusicFreePlugin::AwaitSheetCall(MusicFreePlugin* self, JSContext* ctx,
                                        std::string_view method, JSValue argv[],
                                        int argc,
                                        ResultT (*parse)(const nlohmann::json&)) {
    ResultT result;
    try {
        JSValue res = self->CallMethod(method, argc, argv);
        nlohmann::json j = QuickJsEngine::ValueToJson(ctx, res);
        JS_FreeValue(ctx, res);
        // Await 在「微任务队列已空但 Promise 仍未决」时会返回一个零初始化的
        // JSValue，ValueToJson 会得到数字 0 / null。以前这里直接交给 parse，
        // 结果是 items 为空且 error 也为空，UI 只显示「没有返回曲目」而无从定位。
        if (!j.is_array() && !j.is_object()) {
            std::string got = j.dump();
            if (got.size() > 120) got.resize(120);
            result.error = "插件方法 " + std::string(method) +
                           " 未返回有效对象（得到 " + got +
                           "，通常是 Promise 未决或返回 undefined）";
            return result;
        }
        result = parse(j);
    } catch (const std::exception& e) {
        result.error = e.what();
    }
    return result;
}


std::optional<TopListsResult> MusicFreePlugin::GetTopLists() {
    if (!mounted_ || !meta_.can_top_lists)
        return std::nullopt;
    JSContext* ctx = engine_->context();
    try {
        JSValue res = CallMethod("getTopLists", 0, nullptr);
        nlohmann::json j = QuickJsEngine::ValueToJson(ctx, res);
        JS_FreeValue(ctx, res);
        TopListsResult result;
        result = ParseSheetArray(j);
        return result;
    } catch (const std::exception& e) {
        TopListsResult result;
        result.error = e.what();
        return result;
    }
}

std::optional<SheetTracksResult> MusicFreePlugin::GetTopListDetail(
    const nlohmann::json& sheet, int page) {
    if (!mounted_ || !HasMethod(engine_->context(), instance_, "getTopListDetail"))
        return std::nullopt;
    JSContext* ctx = engine_->context();
    JSValue arg_sheet = QuickJsEngine::JsonToValue(ctx, sheet);
    JSValue arg_page = JS_NewInt32(ctx, page);
    JSValue argv[2] = {arg_sheet, arg_page};
    auto result = AwaitSheetCall<SheetTracksResult>(
        this, ctx, "getTopListDetail", argv, 2, ParseSheetTracks);
    JS_FreeValue(ctx, arg_sheet);
    JS_FreeValue(ctx, arg_page);
    return result;
}

std::optional<SheetTagsResult> MusicFreePlugin::GetRecommendSheetTags() {
    if (!mounted_ || !HasMethod(engine_->context(), instance_, "getRecommendSheetTags"))
        return std::nullopt;
    JSContext* ctx = engine_->context();
    try {
        JSValue res = CallMethod("getRecommendSheetTags", 0, nullptr);
        nlohmann::json j = QuickJsEngine::ValueToJson(ctx, res);
        JS_FreeValue(ctx, res);
        SheetTagsResult result;
        if (j.is_array())
            for (const auto& t : j) result.tags.push_back(SheetTag::FromJson(t));
        return result;
    } catch (const std::exception& e) {
        SheetTagsResult result;
        result.error = e.what();
        return result;
    }
}

std::optional<TopListsResult> MusicFreePlugin::GetRecommendSheetsByTag(
    const nlohmann::json& tag, int page) {
    if (!mounted_ ||
        !HasMethod(engine_->context(), instance_, "getRecommendSheetsByTag"))
        return std::nullopt;
    JSContext* ctx = engine_->context();
    JSValue arg_tag = QuickJsEngine::JsonToValue(ctx, tag);
    JSValue arg_page = JS_NewInt32(ctx, page);
    JSValue argv[2] = {arg_tag, arg_page};
    auto result = AwaitSheetCall<TopListsResult>(
        this, ctx, "getRecommendSheetsByTag", argv, 2, ParseSheetArray);
    JS_FreeValue(ctx, arg_tag);
    JS_FreeValue(ctx, arg_page);
    return result;
}

std::optional<SheetTracksResult> MusicFreePlugin::GetMusicSheetInfo(
    const nlohmann::json& sheet, int page) {
    if (!mounted_ ||
        !HasMethod(engine_->context(), instance_, "getMusicSheetInfo"))
        return std::nullopt;
    JSContext* ctx = engine_->context();
    JSValue arg_sheet = QuickJsEngine::JsonToValue(ctx, sheet);
    JSValue arg_page = JS_NewInt32(ctx, page);
    JSValue argv[2] = {arg_sheet, arg_page};
    auto result = AwaitSheetCall<SheetTracksResult>(
        this, ctx, "getMusicSheetInfo", argv, 2, ParseSheetTracks);
    JS_FreeValue(ctx, arg_sheet);
    JS_FreeValue(ctx, arg_page);
    return result;
}

} // namespace ttplayer::musicfree
