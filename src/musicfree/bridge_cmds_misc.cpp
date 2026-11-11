// 音乐窗 WebView2 JSON 桥 —— 搜索 / 收藏 / 历史 / 配置 命令实现（Task 8）
//
// 职责边界：只做「参数校验 → 复用既有业务函数 → 拼 JSON 回执/推事件」。
// 三条硬约束（对应 spec「功能不减」与 tasks.md SubTask 8.2）：
//   1. 搜索走既有 SearchAllSources（musicfree_app.cpp，逐音源 QuickJS 聚合搜索）；
//   2. 配置/历史读写走既有 settings::LoadRuntimeSettings + settings::SaveWindowState
//      与 musicfree_app.cpp 的 SetAutoUpdateSourcesEnabled —— 落盘文件
//      （TTPlayerRebuild.xml / musicfree/update_state.json）与格式一律不变；
//   3. 不新造任何存储文件；handler 不抛异常，失败统一回 ok:false + 中文 error。
//
// 线程模型（见 musicfree_bridge.h 头注）：
//   · handler 在 UI 线程被调；搜索放在子线程执行，结果经 PushEvent（线程安全）
//     推回页面；回执 reply 允许异步，但页面 5s 超时，故 search.query 先立即
//     回执 accepted，再由后台推 search.result 事件。

#include "bridge_cmds_misc.h"

#include <cstdint>
#include <exception>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ttplayer/app/runtime.h"
#include "ttplayer/core/text.h"
#include "ttplayer/musicfree/musicfree_app.h"
#include "ttplayer/musicfree/musicfree_browser_window.h"
#include "ttplayer/settings/settings.h"

namespace ttplayer::musicfree {

// 复用音乐窗（musicfree_browser_window.cpp）的插件串行锁：每个 MusicFree 插件
// 持有独立 QuickJS runtime，不能被两个线程同时重入，音乐窗的搜索/解析/发现页
// 加载都在「一次插件调用」粒度持这把锁（见该文件 g_plugin_io 定义处注释）。
// 注意：该符号目前位于 musicfree_browser_window.cpp 的匿名命名空间内，
// 需宿主把它移出匿名命名空间（保留名字与类型）并在此头文件 extern 声明后才能链接。
extern std::mutex g_plugin_io;

namespace {

using json = nlohmann::json;

// ───────────────────────── 参数取值（入参来自页面，不可信，一律不抛） ──────────

std::string JsonStr(const json& j, const char* key) {
    if (!j.is_object()) return {};
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return {};
    if (it->is_string()) return it->get<std::string>();
    return it->dump();
}

// cfg.set 的 value：按目标字段类型解析；类型对不上返回 nullopt（调用方回中文错误）。
std::optional<int> AsInt(const json& v) {
    if (v.is_number_integer()) return v.get<int>();
    if (v.is_number_float()) return static_cast<int>(v.get<double>());
    if (v.is_string()) {
        try {
            size_t pos = 0;
            const std::string& s = v.get_ref<const std::string&>();
            const int n = std::stoi(s, &pos);
            if (pos == s.size()) return n;
        } catch (...) {
        }
    }
    return std::nullopt;
}

std::optional<bool> AsBool(const json& v) {
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number()) return v.get<double>() != 0.0;
    if (v.is_string()) {
        const std::string& s = v.get_ref<const std::string&>();
        if (s == "1" || s == "true" || s == "yes" || s == "on") return true;
        if (s == "0" || s == "false" || s == "no" || s == "off") return false;
    }
    return std::nullopt;
}

// 时长格式化（与音乐窗列表一致的 mm:ss；0/负数显示 "-"）。
std::string FormatDurationMs(std::int64_t duration_ms) {
    if (duration_ms <= 0) return "-";
    const std::int64_t seconds = duration_ms / 1000;
    std::string text = std::to_string(seconds / 60) + ":";
    const std::int64_t remainder = seconds % 60;
    if (remainder < 10) text += "0";
    text += std::to_string(remainder);
    return text;
}

// ───────────────────────── 进程内收藏表 ─────────────────────────
//
// 【为什么没有落盘】全库检索确认：既有代码没有任何收藏存储 —— 音乐窗图标栏
// 的「收藏」是占位按钮（musicfree_browser_window.cpp 图标栏注释「其余先占位」），
// 也没有收藏相关的历史文件。spec 硬要求「不改存储格式、不新造存储」，因此
// fav.set 只维护**进程内**状态（回执里带 persisted:false 说明），等宿主提供
// 既有收藏存储（或确认允许新文件）后再接真正的落盘，绝不擅自新建文件。
struct FavoritesStore {
    std::mutex mtx;
    std::map<std::string, json> items;  // "kind\x1fid" → 条目快照（任意 JSON）
};

FavoritesStore& Favorites() {
    static FavoritesStore store;
    return store;
}

std::string FavKey(const std::string& kind, const std::string& id) {
    return kind + "\x1f" + id;
}

// ───────────────────────── 搜索结果 → 页面视图模型 ─────────────────────────
//
// 事件契约（tasks.md Task 8.1 + webui/index.html 渲染期望）：
//   search.result payload = { q, tab?, total, groups: [ { name, items: [...] } ],
//                             diagnostics: { sources, sources_with_hits, errors } }
//   item = { title, artist, cover, album, duration, id, extra: {...} }
//   · title/artist/cover/extra —— Task 8 指定的四个字段；
//   · album/duration/id        —— index.html renderSearch 读 r.title/r.artist/
//                                  r.album/r.duration/r.id，缺一会渲染成空行；
//   · extra 放播放所需的回传信息（platform + 音源 raw 原样对象）：解析播放地址
//     必须把 raw 原样回传（见 musicfree_app.h ResolvePlayUrl 注释）。
json BuildSearchGroups(const std::vector<OnlineSearchHit>& hits) {
    json groups = json::array();
    std::vector<std::string> order;              // 分组出现顺序（按音源）
    std::map<std::string, json> by_platform;     // 音源名 → items 数组
    for (const auto& hit : hits) {
        const std::string& name = hit.platform.empty() ? std::string("未知音源")
                                                       : hit.platform;
        auto it = by_platform.find(name);
        if (it == by_platform.end()) {
            order.push_back(name);
            it = by_platform.emplace(name, json::array()).first;
        }
        json item;
        item["title"] = hit.title;
        item["artist"] = hit.artist;
        item["cover"] = hit.artwork;
        item["album"] = hit.album;
        item["duration"] = FormatDurationMs(hit.duration_ms);
        item["id"] = hit.id;
        json extra;
        extra["platform"] = hit.platform;
        extra["duration_ms"] = hit.duration_ms;
        extra["raw"] = hit.raw;  // 音源原始对象，解析时原样回传
        item["extra"] = std::move(extra);
        it->second.push_back(std::move(item));
    }
    for (const auto& name : order) {
        json group;
        group["name"] = name;
        group["items"] = std::move(by_platform[name]);
        groups.push_back(std::move(group));
    }
    return groups;
}

// ───────────────────────── 配置 / 历史（既有落盘文件） ─────────────────────────
//
// 配置与历史都住在 exe 目录的 TTPlayerRebuild.xml（settings::kSettingsFileName），
// 读写分别复用 settings::LoadRuntimeSettings 与 settings::SaveWindowState：
// 后者是「打开既有文档 → 改同名属性 → 保存」，未知属性原样保留，格式不变。
// 历史节点即配置里的 <Histroy> 元素（拼写是既有持久化格式的一部分，不动）。

json HistoryToJson(const settings::Settings& s) {
    json j;
    j["last_active_page"] = s.history.last_active_page;
    j["sound_path"] = core::WideToUtf8(s.history.sound_path.wstring());
    j["playlist_path"] = core::WideToUtf8(s.history.playlist_path.wstring());
    j["folder"] = core::WideToUtf8(s.history.folder.wstring());
    j["tag_pattern"] = core::WideToUtf8(s.history.tag_pattern);
    j["check_sub_folder"] = s.history.check_sub_folder;
    // 「上次播放」（Player 节点的 PlayingFileName/PlayingTime/ActiveList）——
    // 既有代码里最接近播放历史的持久化，一并放进 history 段只读返回。
    json playing;
    playing["file"] = core::WideToUtf8(s.player.playing_file_name);
    playing["time"] = s.player.playing_time;
    playing["active_playlist"] = s.player.active_playlist;
    j["playing"] = std::move(playing);
    return j;
}

json FavoritesToJson() {
    auto& store = Favorites();
    std::lock_guard<std::mutex> lock(store.mtx);
    json out = json::array();
    for (const auto& kv : store.items) out.push_back(kv.second);
    json j;
    j["persisted"] = false;  // 既有实现没有收藏落盘（见 FavoritesStore 注释）
    j["note"] = "既有实现未提供收藏落盘存储，收藏状态仅本次运行有效";
    j["items"] = std::move(out);
    return j;
}

}  // namespace

// ───────────────────────── 命令注册 ─────────────────────────

void RegisterMiscCommands(MusicFreeBridge& bridge) {
    // ── music.import：打开「导入音源」对话框 ──
    // 与 Win32 界面「导入…」按钮同一实现（musicfree_browser_window.cpp 的
    // kIdImport 音乐分支），这里只做薄封装。对话框是模态的：调用会阻塞到用户
    // 关闭为止，因此页面侧不要依赖本次回执（按超时静默处理即可）。
    bridge.Register("music.import",
                    [](const json&, MusicFreeBridge::ReplyFn reply) {
                        if (MusicFreeOpenImportDialog()) {
                            reply(true, json{{"opened", true}}, {});
                        } else {
                            reply(false, json{},
                                  "音乐窗未打开或正在加载音源，请稍后再试");
                        }
                    });

    // ── search.query：输入 → 既有 QuickJS 音源聚合搜索 → evt search.result ──
    // payload: { q: string, tab?: string }；结果不走 result 而是**推送**事件
    // （搜索常超过页面 5s 超时，先回 accepted 再等事件）。
    bridge.Register("search.query",
                    [&bridge](const json& payload, MusicFreeBridge::ReplyFn reply) {
                        const std::string q = JsonStr(payload, "q");
                        const std::string tab = JsonStr(payload, "tab");
                        if (q.empty()) {
                            reply(false, json{}, "搜索关键词为空（payload.q 缺失）");
                            return;
                        }
                        // 立即回执：工作线程只负责推事件，避免页面 send() 超时。
                        json ack;
                        ack["accepted"] = true;
                        ack["q"] = q;
                        if (!tab.empty()) ack["tab"] = tab;
                        reply(true, std::move(ack), {});

                        std::thread([&bridge, q, tab] {
                            BgThreadGuard guard;  // 退出时由 ShutdownMusicFree 等待
                            if (IsMusicFreeShuttingDown()) return;
                            json out;
                            out["q"] = q;
                            if (!tab.empty()) out["tab"] = tab;
                            try {
                                // 与音乐窗共用插件串行锁；SearchAllSources 内部会
                                // WaitForMusicFreeSources()（首次调用可能等数秒，
                                // 所以必须在子线程）。
                                std::lock_guard<std::mutex> lock(g_plugin_io);
                                OnlineSearchDiagnostics diag;
                                const auto hits =
                                    SearchAllSources(q, {}, 1, &diag);
                                out["groups"] = BuildSearchGroups(hits);
                                out["total"] = hits.size();
                                json d;
                                d["sources"] = diag.sources;
                                d["sources_with_hits"] = diag.sources_with_hits;
                                d["errors"] = diag.errors;
                                out["diagnostics"] = std::move(d);
                            } catch (const std::exception& e) {
                                // 单次搜索失败不崩：空结果 + 诊断文本照样推回。
                                out["groups"] = json::array();
                                out["total"] = 0;
                                json d;
                                d["errors"] =
                                    json::array({std::string("搜索失败: ") + e.what()});
                                out["diagnostics"] = std::move(d);
                            } catch (...) {
                                out["groups"] = json::array();
                                out["total"] = 0;
                                json d;
                                d["errors"] =
                                    json::array({std::string("搜索失败: 未知错误")});
                                out["diagnostics"] = std::move(d);
                            }
                            bridge.PushEvent("search.result", out);
                        }).detach();
                    });

    // ── fav.set：收藏 / 取消收藏（kind+id 定位条目；缺 on 则为读取） ──
    // payload: { kind: string, id: string, on: bool, item?: object }。
    // 收藏存储现状见 FavoritesStore 注释：既有代码无落盘收藏，这里只记进程内，
    // 回执带 persisted:false，不写任何文件（不新造存储、不改格式）。
    bridge.Register("fav.set", [](const json& payload,
                                  MusicFreeBridge::ReplyFn reply) {
        const std::string kind = JsonStr(payload, "kind");
        const std::string id = JsonStr(payload, "id");
        if (kind.empty() || id.empty()) {
            reply(false, json{}, "收藏参数不完整（payload 需要 kind 与 id）");
            return;
        }
        auto& store = Favorites();
        const std::string key = FavKey(kind, id);
        const bool has_on = payload.is_object() && payload.contains("on") &&
                            !payload.at("on").is_null();
        // 类型校验放在加锁前：失败时直接回执，不持有收藏锁去调 reply。
        std::optional<bool> parsed;
        if (has_on) {
            parsed = AsBool(payload.at("on"));
            if (!parsed) {
                reply(false, json{}, "payload.on 需要布尔值（true=收藏）");
                return;
            }
        }
        bool on = false;
        {
            std::lock_guard<std::mutex> lock(store.mtx);
            if (!parsed) {
                // 未带 on：当作收藏状态读取。
                on = store.items.contains(key);
            } else {
                on = *parsed;
                if (on) {
                    json entry;
                    if (payload.contains("item") && payload.at("item").is_object())
                        entry = payload.at("item");
                    else
                        entry = json::object();
                    entry["kind"] = kind;
                    entry["id"] = id;
                    store.items[key] = std::move(entry);
                } else {
                    store.items.erase(key);
                }
            }
        }
        json result;
        result["kind"] = kind;
        result["id"] = id;
        result["on"] = on;
        result["persisted"] = false;
        result["note"] = "既有实现未提供收藏落盘存储，收藏状态仅本次运行有效";
        reply(true, std::move(result), {});
    });

    // ── cfg.get：读既有配置 / 历史 ──
    // payload: { section?: "history" | "auto_update" | "favorites" | "all" }
    //（缺省 all）。文件与格式：TTPlayerRebuild.xml、musicfree/update_state.json。
    bridge.Register("cfg.get", [](const json& payload,
                                  MusicFreeBridge::ReplyFn reply) {
        std::string section = JsonStr(payload, "section");
        if (section.empty()) section = "all";
        try {
            json result;
            if (section == "history" || section == "all") {
                // 既有读取路径（含 TTPlayer.xml → TTPlayerRebuild.xml 的
                // 一次性导入，与启动时同一函数，不会改格式）。
                const auto s = settings::LoadRuntimeSettings(
                    app::RuntimePath({}));
                if (section == "history") {
                    result = HistoryToJson(s);
                } else {
                    result["history"] = HistoryToJson(s);
                }
            }
            if (section == "auto_update" || section == "all") {
                // 既有 musicfree/update_state.json 的读口（musicfree_app.cpp）。
                json auto_update;
                auto_update["auto_check"] = AutoUpdateSourcesEnabled();
                if (section == "auto_update")
                    result = std::move(auto_update);
                else
                    result["auto_update"] = std::move(auto_update);
            }
            if (section == "favorites" || section == "all") {
                json fav = FavoritesToJson();
                if (section == "favorites")
                    result = std::move(fav);
                else
                    result["favorites"] = std::move(fav);
            }
            if (result.empty() && section != "all") {
                reply(false, json{}, "未知配置段: " + section +
                                         "（可用 history / auto_update / favorites / all）");
                return;
            }
            reply(true, std::move(result), {});
        } catch (const std::exception& e) {
            reply(false, json{}, std::string("读取配置失败: ") + e.what());
        } catch (...) {
            reply(false, json{}, "读取配置失败: 未知错误");
        }
    });

    // ── cfg.set：写既有配置 ──
    // payload: { section:"auto_update", auto_check: bool }
    //          { section:"history", key, value }   （key 白名单见下）
    // 写路径与既有保存完全一致：LoadRuntimeSettings → 改字段 →
    // settings::SaveWindowState（打开既有 XML 改同名属性后保存，格式不变）；
    // auto_update 走 SetAutoUpdateSourcesEnabled（update_state.json 格式不变）。
    bridge.Register("cfg.set", [](const json& payload,
                                  MusicFreeBridge::ReplyFn reply) {
        const std::string section = JsonStr(payload, "section");
        try {
            if (section == "auto_update") {
                if (!payload.is_object() || !payload.contains("auto_check")) {
                    reply(false, json{}, "缺少 auto_check（布尔值）");
                    return;
                }
                auto v = AsBool(payload.at("auto_check"));
                if (!v) {
                    reply(false, json{}, "auto_check 需要布尔值");
                    return;
                }
                SetAutoUpdateSourcesEnabled(*v);  // 落 update_state.json，格式不变
                json result;
                result["section"] = section;
                result["auto_check"] = *v;
                reply(true, std::move(result), {});
                return;
            }

            if (section == "history") {
                const std::string key = JsonStr(payload, "key");
                if (!payload.is_object() || !payload.contains("key") ||
                    key.empty()) {
                    reply(false, json{}, "缺少 key（要写入的历史字段名）");
                    return;
                }
                if (!payload.contains("value") || payload.at("value").is_null()) {
                    reply(false, json{}, "缺少 value");
                    return;
                }
                const json& value = payload.at("value");
                // 只允许写 <Histroy> 节点里类型明确的既有字段（白名单），
                // 其余一律拒绝，避免造出配置里不存在的属性。
                const bool known =
                    key == "last_active_page" || key == "check_sub_folder" ||
                    key == "sound_path" || key == "playlist_path" ||
                    key == "folder" || key == "tag_pattern";
                if (!known) {
                    reply(false, json{},
                          "不支持的历史配置项: " + key +
                              "（可用 last_active_page / check_sub_folder / "
                              "sound_path / playlist_path / folder / tag_pattern）");
                    return;
                }

                auto s = settings::LoadRuntimeSettings(app::RuntimePath({}));
                if (s.source_path.empty() ||
                    !std::filesystem::exists(s.source_path)) {
                    // 配置文件还没被创建时不代创建（保持与启动流程一致）。
                    reply(false, json{}, "配置文件不存在，拒绝写入");
                    return;
                }
                if (key == "last_active_page") {
                    auto v = AsInt(value);
                    if (!v) {
                        reply(false, json{}, "last_active_page 需要整数");
                        return;
                    }
                    s.history.last_active_page = *v;
                } else if (key == "check_sub_folder") {
                    auto v = AsBool(value);
                    if (!v) {
                        reply(false, json{}, "check_sub_folder 需要布尔值");
                        return;
                    }
                    s.history.check_sub_folder = *v;
                } else {
                    if (!value.is_string()) {
                        reply(false, json{}, key + " 需要字符串值");
                        return;
                    }
                    const std::wstring text =
                        core::Utf8ToWide(value.get_ref<const std::string&>());
                    if (key == "sound_path") s.history.sound_path = text;
                    else if (key == "playlist_path") s.history.playlist_path = text;
                    else if (key == "folder") s.history.folder = text;
                    else s.history.tag_pattern = text;
                }
                // 既有保存路径：打开既有文档改属性后保存，未知属性原样保留。
                settings::SaveWindowState(s.source_path, s);
                json result;
                result["section"] = section;
                result["key"] = key;
                result["value"] = value;
                reply(true, std::move(result), {});
                return;
            }

            reply(false, json{},
                  "不支持写入的配置段: " +
                      (section.empty() ? std::string("(空)") : section) +
                      "（可用 history / auto_update）");
        } catch (const std::exception& e) {
            reply(false, json{}, std::string("写入配置失败: ") + e.what());
        } catch (...) {
            reply(false, json{}, "写入配置失败: 未知错误");
        }
    });
}

}  // namespace ttplayer::musicfree
