#pragma once

// MusicFree 兼容层 —— 单个插件封装
//
// 对应 MusicFree 的 Plugin 类：持有独立 QuickJsEngine（独立 runtime/context，
// 与 MusicFree 共享全局不同，这里用隔离避免插件间污染），挂载插件 JS 后把
// search / getMediaSource / getLyric 暴露为 C++ 方法。

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "ttplayer/musicfree/musicfree_types.h"
#include "ttplayer/musicfree/quickjs_engine.h"

namespace ttplayer::musicfree {

class MusicFreePlugin {
public:
    // 仅创建引擎（尚未挂载）。source_url 仅用于记录。
    explicit MusicFreePlugin(std::string source_url);
    ~MusicFreePlugin();

    MusicFreePlugin(const MusicFreePlugin&) = delete;
    MusicFreePlugin& operator=(const MusicFreePlugin&) = delete;

    // 配置网络与依赖库目录（挂载前调用）。
    void Configure(NetworkConfig network, std::filesystem::path lib_dir);

    // 挂载插件 JS。返回是否成功（失败时 mounted_=false，可从 last_error() 取原因）。
    bool Mount(std::string_view js_code, std::string_view app_version = "0.1.0");

    const PluginMeta& meta() const { return meta_; }
    bool IsMounted() const { return mounted_; }
    const std::string& last_error() const { return last_error_; }

    // 在线搜索。type 为 MusicFree 媒体类型（"music"/"album"/...），默认 music。
    SearchResult Search(std::string_view query, int page = 1,
                        std::string_view type = "music");

    // 解析真实播放地址。失败返回 nullopt；error 非空时写入失败原因，
    // 便于音乐窗把「为什么解析不出来」直接显示给用户（而不是只说一句失败）。
    std::optional<MediaSource> GetMediaSource(const MusicItem& item,
                                              QualityKey quality = QualityKey::kStandard,
                                              std::string* error = nullptr);

    // 获取歌词。
    std::optional<LyricResult> GetLyric(const MusicItem& item);

    // ---- 内容浏览（复刻 MusicFree IPlugin 的内容方法）----
    // 洛雪音源协议不提供这类能力；以下全部来自 MusicFree 插件。返回 nullopt 表示
    // 插件未挂载 / 不支持 / 调用失败（失败时 error 写入 last_error_）。

    // 排行榜列表（getTopLists）。结果可含分组（group），扁平化后每条带 platform。
    std::optional<TopListsResult> GetTopLists();

    // 排行榜详情：给定歌单原始对象，取它的曲目（getTopListDetail）。
    std::optional<SheetTracksResult> GetTopListDetail(const nlohmann::json& sheet,
                                                     int page = 1);

    // 推荐歌单标签（getRecommendSheetTags）。
    std::optional<SheetTagsResult> GetRecommendSheetTags();

    // 某标签下的推荐歌单（getRecommendSheetsByTag）。
    std::optional<TopListsResult> GetRecommendSheetsByTag(const nlohmann::json& tag,
                                                         int page = 1);

    // 歌单详情：给定歌单原始对象，取它的曲目（getMusicSheetInfo）。
    std::optional<SheetTracksResult> GetMusicSheetInfo(const nlohmann::json& sheet,
                                                     int page = 1);

private:
    // 调用插件方法并 await；返回已解析的 JSValue（调用方释放），失败抛异常。
    JSValue CallMethod(std::string_view method, int argc, JSValueConst* argv);

    // 详情类内容方法（getTopListDetail / getMusicSheetInfo / getRecommendSheetsByTag）
    // 的通用 await + 解析；声明为私有静态成员以便访问私有的 CallMethod。
    template <typename ResultT>
    static ResultT AwaitSheetCall(MusicFreePlugin* self, JSContext* ctx,
                                  std::string_view method, JSValue argv[], int argc,
                                  ResultT (*parse)(const nlohmann::json&));

    std::unique_ptr<QuickJsEngine> engine_;
    PluginMeta meta_;
    std::string source_url_;
    std::string last_error_;
    bool mounted_ = false;
    JSValue instance_ = JS_UNDEFINED; // 插件导出的 IPluginDefine（引擎托管，生命周期随 engine_）
};

} // namespace ttplayer::musicfree
