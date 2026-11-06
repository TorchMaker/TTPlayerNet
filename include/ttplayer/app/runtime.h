#pragma once

#include "ttpcomm_api.h"
#include "ttplayer/plugins/plugin_manager.h"

#include <filesystem>
#include <windows.h>

namespace ttplayer::app {
std::filesystem::path RuntimePath(const std::filesystem::path& relative);
std::filesystem::path FindRuntimePath(const std::filesystem::path& relative);

class TtpCommRuntime {
public:
    ~TtpCommRuntime();
    bool Initialize();
    void Shutdown();
    [[nodiscard]] TtpCommApi& Api() noexcept { return api_; }
private:
    TtpCommApi api_{};
};

class ResourceRuntime {
public:
    ~ResourceRuntime();
    bool Initialize();
    void Shutdown();
    [[nodiscard]] HMODULE Module() const noexcept { return module_; }
private:
    HMODULE module_{};
};

class SoundLibraryRuntime {
public:
    ~SoundLibraryRuntime();
    // 只登记 AddIn 目录，不加载任何 DLL。窗口显示后由 EnsureLoaded() 完成真正的
    // 枚举 + LoadLibrary；音效库越多这一段越慢，且播放器首屏并不依赖它。
    void SetDirectory(std::filesystem::path addin_directory);
    // 幂等。必须在主线程调用：原版 AddIn 的工厂函数会注册窗口类、初始化 COM，
    // 放到工作线程会破坏这些隐式线程约束。失败也不会重复重试。
    HRESULT EnsureLoaded();
    // 兼容路径：无窗口的自检 / --reg / --unreg 需要启动即完整加载。
    HRESULT Initialize(const std::filesystem::path& addin_directory);
    [[nodiscard]] bool Loaded() const noexcept { return loaded_; }
    void Shutdown();
    [[nodiscard]] size_t DiscoveredPluginCount() const noexcept {
        return manager_.RegisteredPluginCount();
    }
    [[nodiscard]] const std::vector<plugins::ReaderFormat>& ReaderFormats() const noexcept {
        return manager_.ReaderFormats();
    }
    [[nodiscard]] const std::vector<plugins::PluginInfo>& Plugins() const noexcept {
        return manager_.Plugins();
    }
    [[nodiscard]] const plugins::PluginManager& Manager() const noexcept {
        return manager_;
    }
private:
    plugins::PluginManager manager_;
    std::filesystem::path directory_;
    bool loaded_{};
};

class ThreadMessageHooksRuntime {
public:
    ~ThreadMessageHooksRuntime();
    bool Install();
    void Shutdown();
private:
    static LRESULT CALLBACK CallWndProcHook(int code, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK CbtHook(int code, WPARAM wparam, LPARAM lparam);
    HHOOK call_window_hook_{};
    HHOOK cbt_hook_{};
};
} // namespace ttplayer::app
