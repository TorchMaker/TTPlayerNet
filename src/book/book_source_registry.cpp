#include "ttplayer/book/book_source_registry.h"

#include "../plugins/content_plugin_loader.h"

namespace ttplayer::book {

namespace {

using ttplayer::plugins::AppendContentLog;
using ttplayer::plugins::FindContentPlugins;
using ttplayer::plugins::GetPluginFactory;
using ttplayer::plugins::SetPluginError;

std::string Utf8(const std::wstring& value) {
    return ttplayer::core::WideToUtf8(value);
}

void WINAPI EmitItem(void* ctx, const TtpBookItem* item) {
    auto* out = static_cast<std::vector<BookItem>*>(ctx);
    if (!ctx || !item || !item->id || !*item->id) return;
    BookItem value;
    value.id = item->id;
    if (item->title) value.title = item->title;
    if (item->author) value.author = item->author;
    if (item->cover) value.cover = item->cover;
    if (item->intro) value.intro = item->intro;
    if (item->extra) value.extra = item->extra;
    out->push_back(std::move(value));
}

void WINAPI EmitChapter(void* ctx, const TtpBookChapter* chapter) {
    auto* out = static_cast<std::vector<BookChapter>*>(ctx);
    if (!ctx || !chapter || !chapter->url || !*chapter->url) return;
    BookChapter value;
    value.title = chapter->title ? chapter->title : "";
    value.url = chapter->url;
    value.index = chapter->index;
    out->push_back(std::move(value));
}

}  // namespace

BookSourceRegistry& BookSourceRegistry::Instance() {
    static BookSourceRegistry instance;
    return instance;
}

BookSourceRegistry::~BookSourceRegistry() {
    for (auto& plugin : plugins_) {
        if (plugin.module) FreeLibrary(plugin.module);
    }
}

void BookSourceRegistry::LoadAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (loaded_) return;
    loaded_ = true;
    for (const auto& path : FindContentPlugins(L"ttp_b_")) {
        Plugin plugin;
        plugin.module = LoadLibraryW(path.c_str());
        if (!plugin.module) {
            AppendContentLog(L"book", "LOAD fail " + Utf8(path.filename().wstring()) +
                                          " err=" +
                                          std::to_string(GetLastError()));
            continue;
        }
        using Factory = HRESULT(WINAPI*)(TtpBookSourceApi*);
        auto factory = reinterpret_cast<Factory>(
            GetPluginFactory(plugin.module, "ttpGetBookSource"));
        if (!factory) {
            AppendContentLog(L"book", "LOAD no-factory " +
                                          Utf8(path.filename().wstring()));
            FreeLibrary(plugin.module);
            continue;
        }
        TtpBookSourceApi api{};
        if (FAILED(factory(&api)) || !api.api_version ||
            api.api_version() != TTPLAYER_BOOK_SOURCE_ABI_VERSION) {
            AppendContentLog(L"book", "LOAD abi-mismatch " +
                                          Utf8(path.filename().wstring()));
            FreeLibrary(plugin.module);
            continue;
        }
        TtpBookSourceInfo info{};
        if (api.info) api.info(&info);
        plugin.info.id = info.id ? info.id : "";
        plugin.info.name = info.name ? info.name : plugin.info.id;
        plugin.info.version = info.vendor ? info.vendor : "";
        plugin.info.is_audio = info.is_audio != 0;
        plugin.info.path = path.wstring();
        plugin.api = api;
        plugin.usable = !plugin.info.id.empty();
        if (plugin.usable) {
            AppendContentLog(L"book", "LOAD ok id=" + plugin.info.id + " name=" +
                                          plugin.info.name + " file=" +
                                          Utf8(path.filename().wstring()));
            plugins_.push_back(std::move(plugin));
        } else {
            AppendContentLog(L"book", "LOAD no-id " +
                                          Utf8(path.filename().wstring()));
            FreeLibrary(plugin.module);
        }
    }
    AppendContentLog(L"book", "LOAD total=" + std::to_string(plugins_.size()));
}

std::vector<BookSourcePluginInfo> BookSourceRegistry::Sources() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<BookSourcePluginInfo> out;
    out.reserve(plugins_.size());
    for (const auto& plugin : plugins_) out.push_back(plugin.info);
    return out;
}

BookSourceRegistry::Plugin* BookSourceRegistry::FindLocked(
    const std::string& plugin_id) {
    for (auto& plugin : plugins_) {
        if (plugin.usable && plugin.info.id == plugin_id) return &plugin;
    }
    return nullptr;
}

bool BookSourceRegistry::Search(const std::string& plugin_id,
                                const std::string& keyword, int page,
                                std::vector<BookItem>* out, std::string* error,
                                int timeout_ms) {
    if (!out) return false;
    out->clear();
    LoadAll();
    std::lock_guard<std::mutex> lock(mutex_);
    Plugin* plugin = FindLocked(plugin_id);
    if (!plugin || !plugin->api.search) {
        if (error) *error = "未安装听书源插件: " + plugin_id;
        AppendContentLog(L"book", "SEARCH miss-plugin id=" + plugin_id);
        return false;
    }
    const std::wstring wide_keyword = ttplayer::core::Utf8ToWide(keyword);
    char buffer[512]{};
    const int rc = plugin->api.search(wide_keyword.c_str(), page, EmitItem, out,
                                      buffer, sizeof(buffer), timeout_ms);
    if (rc != 0) {
        if (error) *error = buffer[0] ? buffer : "搜索失败";
        AppendContentLog(L"book", "SEARCH fail id=" + plugin_id + " q=" +
                                      keyword + " err=" +
                                      (buffer[0] ? buffer : "unknown"));
        return false;
    }
    AppendContentLog(L"book", "SEARCH ok id=" + plugin_id + " q=" + keyword +
                                  " items=" + std::to_string(out->size()));
    return true;
}

bool BookSourceRegistry::Toc(const std::string& plugin_id,
                             const std::string& book_id,
                             std::vector<BookChapter>* out, std::string* error,
                             int timeout_ms) {
    if (!out) return false;
    out->clear();
    LoadAll();
    std::lock_guard<std::mutex> lock(mutex_);
    Plugin* plugin = FindLocked(plugin_id);
    if (!plugin || !plugin->api.toc) {
        if (error) *error = "未安装听书源插件: " + plugin_id;
        AppendContentLog(L"book", "TOC miss-plugin id=" + plugin_id);
        return false;
    }
    char buffer[512]{};
    const int rc = plugin->api.toc(book_id.c_str(), EmitChapter, out, buffer,
                                   sizeof(buffer), timeout_ms);
    if (rc != 0) {
        if (error) *error = buffer[0] ? buffer : "获取目录失败";
        AppendContentLog(L"book", "TOC fail id=" + plugin_id + " book=" +
                                      book_id + " err=" +
                                      (buffer[0] ? buffer : "unknown"));
        return false;
    }
    AppendContentLog(L"book", "TOC ok id=" + plugin_id + " book=" + book_id +
                                  " chapters=" + std::to_string(out->size()));
    return true;
}

bool BookSourceRegistry::Content(const std::string& plugin_id,
                                 const std::string& chapter_url,
                                 std::string* text, std::string* error,
                                 int timeout_ms) {
    if (!text) return false;
    text->clear();
    LoadAll();
    std::lock_guard<std::mutex> lock(mutex_);
    Plugin* plugin = FindLocked(plugin_id);
    if (!plugin || !plugin->api.content) {
        if (error) *error = "未安装听书源插件: " + plugin_id;
        AppendContentLog(L"book", "CONTENT miss-plugin id=" + plugin_id);
        return false;
    }
    char buffer[512]{};
    char* result = plugin->api.content(chapter_url.c_str(), buffer,
                                       sizeof(buffer), timeout_ms);
    if (!result) {
        if (error) *error = buffer[0] ? buffer : "获取正文失败";
        AppendContentLog(L"book", "CONTENT fail id=" + plugin_id + " url=" +
                                      chapter_url + " err=" +
                                      (buffer[0] ? buffer : "unknown"));
        return false;
    }
    *text = result;
    if (plugin->api.free_text) plugin->api.free_text(result);
    AppendContentLog(L"book", "CONTENT ok id=" + plugin_id + " bytes=" +
                                  std::to_string(text->size()));
    return true;
}

}  // namespace ttplayer::book
