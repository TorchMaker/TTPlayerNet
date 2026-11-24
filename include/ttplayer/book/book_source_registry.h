#pragma once

// 听书 / 广播内容源注册表：加载 <exe>\AddIn\ttp_b_*.dll
//（C ABI 见 ttplayer/book/book_source_api.h）。

#include <mutex>
#include <string>
#include <vector>

#include <windows.h>

#include "ttplayer/book/book_source_api.h"

namespace ttplayer::book {

struct BookItem {
    std::string id;
    std::string title;
    std::string author;
    std::string cover;
    std::string intro;
    std::string extra;
};

struct BookChapter {
    std::string title;
    std::string url;
    int index{};
};

struct BookSourcePluginInfo {
    std::string id;
    std::string name;
    std::string version;
    bool is_audio{};
    std::wstring path;
};

class BookSourceRegistry {
public:
    static BookSourceRegistry& Instance();

    void LoadAll();

    std::vector<BookSourcePluginInfo> Sources() const;

    bool Search(const std::string& plugin_id, const std::string& keyword,
                int page, std::vector<BookItem>* out, std::string* error,
                int timeout_ms = 20000);

    bool Toc(const std::string& plugin_id, const std::string& book_id,
             std::vector<BookChapter>* out, std::string* error,
             int timeout_ms = 20000);

    bool Content(const std::string& plugin_id, const std::string& chapter_url,
                 std::string* text, std::string* error, int timeout_ms = 20000);

private:
    BookSourceRegistry() = default;
    ~BookSourceRegistry();
    BookSourceRegistry(const BookSourceRegistry&) = delete;
    BookSourceRegistry& operator=(const BookSourceRegistry&) = delete;

    struct Plugin {
        HMODULE module{};
        BookSourcePluginInfo info;
        TtpBookSourceApi api{};
        bool usable{};
    };

    Plugin* FindLocked(const std::string& plugin_id);
    const Plugin* FindLocked(const std::string& plugin_id) const;

    std::vector<Plugin> plugins_;
    bool loaded_{};
    mutable std::mutex mutex_;
};

}  // namespace ttplayer::book
