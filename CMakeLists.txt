cmake_minimum_required(VERSION 3.24)
project(TTPlayerRebuild VERSION 0.1.0 LANGUAGES C CXX)

option(BUILD_TESTING "Build local, unpublished tests" OFF)
include(CTest)
option(TTPLAYER_STAGE_RUNTIME "Rebuild/copy local original DLLs, skins and configuration beside the EXE" ON)
# 单一兼容构建：一个 EXE 同时兼容 Win7/Win8/8.1 与 Win10/11。
# 兼容机制全部运行时自适应（VC-LTL 静态 CRT、视频层静态编入、WebView2Loader
# 动态加载、按系统版本选 WebView2 Runtime + 缺失自动安装），所以不需要再维护
# 一份「现代版」与一份「Win7 版」两个 EXE。
option(TTPLAYER_LEGACY_WINDOWS "Build a single EXE compatible with both Win7 and Win10+ (no separate modern build)" ON)
option(TTPLAYER_OPTIMIZE_SIZE "Optimize Release player size, retaining fast audio processing" ON)
option(TTPLAYER_BUILD_LIBRARY_TESTS "Build local media library regression tests" OFF)
option(TTPLAYER_BUILD_NAVIGATION_TESTS "Build local playback navigation regression tests" OFF)
option(TTPLAYER_BUILD_INFO_TESTS "Build local playlist information regression tests" OFF)
option(TTPLAYER_BUILD_WHEEL_TESTS "Build local playlist mouse wheel tests" OFF)
option(TTPLAYER_BUILD_PREVIEW_TESTS "Build local taskbar album preview tests" OFF)

if(NOT WIN32)
  message(FATAL_ERROR "TTPlayerRebuild currently targets Windows")
endif()

# The recovered resource/communication DLLs and legacy add-in ABI are 32-bit.
# Refuse a generator that would create a host process unable to load them.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR
    "TTPlayerRebuild requires a Win32/x86 generator (configure with -A Win32)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(TTPLAYER_LEGACY_WINDOWS)
  include(cmake/legacy_windows.cmake)
endif()
include(cmake/wtl.cmake)

# Re-evaluate at build time, not just when CMake configures. The generator
# updates the header only if the Beijing calendar date changes, so same-day
# incremental builds do not recompile the options window unnecessarily.
find_program(TTPLAYER_POWERSHELL NAMES powershell pwsh REQUIRED)
set(TTPLAYER_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_custom_target(ttplayer_build_date
  COMMAND "${TTPLAYER_POWERSHELL}" -NoProfile -NonInteractive -ExecutionPolicy Bypass
          -File "${CMAKE_CURRENT_SOURCE_DIR}/cmake/write_build_date.ps1"
          -OutputPath "${TTPLAYER_GENERATED_INCLUDE_DIR}/ttplayer/build_date.h"
  BYPRODUCTS "${TTPLAYER_GENERATED_INCLUDE_DIR}/ttplayer/build_date.h"
  COMMENT "Updating Beijing build date"
  VERBATIM
)

# ---------------------------------------------------------------------------
# MusicFree 音源兼容层依赖：QuickJS-NG（嵌 C 引擎，用于运行 MusicFree 插件的 JS）
# 仅构建核心静态库（不含 qjs/qjsc 可执行文件）。
# ---------------------------------------------------------------------------
add_library(qjs STATIC
  third_party/quickjs-0.9.0/cutils.c
  third_party/quickjs-0.9.0/libbf.c
  third_party/quickjs-0.9.0/libregexp.c
  third_party/quickjs-0.9.0/libunicode.c
  third_party/quickjs-0.9.0/quickjs.c
)
target_include_directories(qjs PUBLIC third_party/quickjs-0.9.0)
target_compile_definitions(qjs PRIVATE _GNU_SOURCE WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0601)
# quickjs-ng 假设 8MB 栈；MSVC 默认 1MB 会导致深递归崩溃。
if(MSVC)
  # quickjs 使用 <stdatomic.h>（要求 C11 及以上）；MSVC 对 .c 默认标准过低会报 C1189。
  target_compile_options(qjs PRIVATE /W0 /std:c11)
  target_link_options(qjs PUBLIC /STACK:8388608)
else()
  target_compile_options(qjs PRIVATE -w)
endif()

add_library(ttplayer_core
  src/i18n/i18n.cpp
  src/ui/wtl_runtime.cpp
  src/ui/wtl_dialogs.cpp
  src/ui/wtl_menu.cpp
  src/app/dsp_worker.cpp
  src/app/file_info_worker.cpp
  src/app/output_device_worker.cpp
  src/audio/archive_member.cpp
  src/audio/asio_sink.cpp
  src/audio/audio_engine.cpp
  src/audio/online_stream_descriptor.cpp
  src/audio/http_stream_source.cpp
  src/audio/builtin_file_info.cpp
  src/audio/cue_sheet.cpp
  src/audio/file_encoder.cpp
  src/audio/kernel_streaming_sink.cpp
  src/audio/legacy_windows_source.cpp
  src/audio/midi_player.cpp
  src/audio/mp3pro_source.cpp
  src/audio/native_output_contract.cpp
  src/audio/pcm_output_transform.cpp
  src/audio/replay_gain_scanner.cpp
  src/audio/winamp_dsp.cpp
  src/core/text.cpp
  src/integrations/discord_presence.cpp
  src/integrations/discord_track_presence.cpp
  src/lyrics/lrc_parser.cpp
  src/lyrics/association.cpp
  src/lyrics/local_search.cpp
  src/lyrics/online_search.cpp
  src/lyrics/service_catalog.cpp
  src/lyrics/lyric_http.cpp
  src/musicfree/quickjs_engine.cpp
  src/musicfree/musicfree_http.cpp
  src/musicfree/webview2_runtime.cpp
  src/musicfree/musicfree_plugin.cpp
  src/musicfree/musicfree_source_manager.cpp
  src/musicfree/musicfree_import_dialog.cpp
  src/musicfree/musicfree_browser_window.cpp
  src/musicfree/musicfree_app.cpp
  src/musicfree/musicfree_artwork.cpp
  src/musicfree/source_protocol.cpp
  src/musicfree/lx_source_adapter.cpp
  src/musicfree/rest_source_adapter.cpp
  src/musicfree/source_protocol_registry.cpp
  # 视频层整体编译进独立 DLL ttplayer_video（见下方 add_library），不再进主静态库，
  # 避免十几个 spider 引擎把主程序撑大；对外的 C++ API 经 TTPLAYER_VIDEO_API 导出。
  src/playlist/playlist.cpp
  src/playlist/playback_order.cpp
  src/playlist/playlist_store.cpp
  src/platform/optional_windows_api.cpp
  src/plugins/plugin_manager.cpp
  src/live/live_source_registry.cpp
  src/book/book_source_registry.cpp
  src/tts/tts_provider_registry.cpp
  src/settings/file_association.cpp
  src/settings/settings.cpp
  src/settings/runtime_settings.cpp
  src/skin/default_colors.cpp
  src/skin/skin.cpp
  src/skin/skin_image.cpp
  src/skin/skin_package.cpp
  src/ui/dpi_scale.cpp
  src/ui/desktop_lyrics.cpp
  src/ui/directory_change_monitor.cpp
  src/ui/file_info_probe_client.cpp
  src/ui/player_window.cpp
  src/ui/player_window_skin_animation.cpp
  src/ui/player_window_taskbar.cpp
  src/ui/taskbar_playback.cpp
  src/ui/taskbar_preview.cpp
  src/ui/player_window_equalizer.cpp
  src/ui/player_window_dpi_selftest.cpp
  src/ui/player_window_file_intake.cpp
  src/ui/output_devices.cpp
  src/ui/modern_file_dialog.cpp
  src/ui/player_control_packet.cpp
  src/ui/player_window_lyrics.cpp
  src/ui/lyric_association_dialog.cpp
  src/ui/lyric_upload_window.cpp
    src/ui/player_window_lyric_search.cpp
    src/ui/player_window_lyric_services.cpp
  src/ui/player_window_lyric_input.cpp
  src/ui/player_window_library.cpp
  src/ui/player_window_options.cpp
  src/ui/player_window_playlist.cpp
  src/ui/player_window_playlist_native.cpp
  src/ui/player_window_playlist_info.cpp
  src/ui/player_window_playlist_network.cpp
  src/ui/player_window_playlist_properties.cpp
  src/ui/player_window_playlist_search.cpp
  src/ui/playlist_network_commands.cpp
  src/ui/playlist_transforms.cpp
  src/ui/player_window_visual.cpp
  src/ui/album_background.cpp
  src/ui/cover_image.cpp
  src/ui/image_decoder_registry.cpp
  src/ui/player_window_visual_options.cpp
  src/ui/shell_send_to.cpp
  src/ui/window_drag.cpp
)
target_include_directories(ttplayer_core
  PUBLIC include
         third_party/ttpcomm-sdk/include
         third_party/ttpres-sdk/include
         third_party/ttp-osu-sdk/include
  PRIVATE "${TTPLAYER_GENERATED_INCLUDE_DIR}"
)
# musicfree_browser_window.cpp 挂接音乐窗 WebView2 宿主时要包含宿主头
#（宿主头内含 WebView2.h），故主库编译单元也需要 WebView2 头文件搜索路径。
# 只加头路径不加链接：WebView2Loader 的链接只落在主 exe 上（见下方
# ttplayer_rebuild 的接线），架构只用 build/native/x86，不引入任何下载。
target_include_directories(ttplayer_core PRIVATE
  ../../third_party/webview2/build/native/include
)
add_dependencies(ttplayer_core ttplayer_build_date)
target_compile_definitions(ttplayer_core PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WTL_DPI_SUPPORT)
target_compile_options(ttplayer_core PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(ttplayer_core PUBLIC
  ttplayer_wtl_headers
  qjs
  winmm ole32 oleaut32 comsuppw uuid strmiids msimg32 shell32 shlwapi comctl32 comdlg32
  windowscodecs gdiplus dsound setupapi dwmapi uxtheme
  mfplat mfreadwrite mf mfuuid propsys winhttp
)

if(NOT TTPLAYER_LEGACY_WINDOWS)
  target_sources(ttplayer_core PRIVATE src/ui/system_media_controls.cpp)
  target_link_libraries(ttplayer_core PUBLIC runtimeobject shcore)
  if(BUILD_TESTING)
    add_executable(system_media_controls_tests tests/ui/system_media_controls_tests.cpp)
    target_compile_definitions(system_media_controls_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(system_media_controls_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_link_libraries(system_media_controls_tests PRIVATE ttplayer_core)
    add_test(NAME system_media_controls_tests COMMAND system_media_controls_tests)
    set_tests_properties(system_media_controls_tests PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
  endif()
endif()

# ─────────────────────────────────────────────────────────────────────────────
# 视频层独立 DLL：视频源管理 / CMS 与直播拉取 / 地址解析 / spider 引擎 / 播放窗
# 整体编译进 ttplayer_video.dll，不再进主静态库 —— 十几个 spider 引擎不再撑大
# 主程序。对外公开 API 经 TTPLAYER_VIDEO_API（video_export.h）导出；同时提供
# C ABI 插件入口 ttpGetVideoModule（video_module_api.h），未来新增内容类型按
# query() 扩展点登记即可，不改 ABI。
# 依赖方向：ttplayer_video → ttplayer_core（静态，仅拉入 text / musicfree_http
# 等无状态符号）；主 exe 同时链接两者（core 在前、video 在后，未解析符号由
# video 的导入库补齐）。TTPLAYER_VIDEO_BUILDING_DLL 使导出宏展开为 dllexport。
# ─────────────────────────────────────────────────────────────────────────────
set(TTPLAYER_VIDEO_SOURCES
  src/video/video_config.cpp
  src/video/video_cms.cpp
  src/video/video_resolve.cpp
  src/video/video_player_js.cpp
  src/video/video_source_manager.cpp
  src/video/video_source_dialog.cpp
  src/video/video_player_window.cpp
  src/video/video_history.cpp
  src/video/video_protocol_registry.cpp
  src/video/video_module.cpp
  src/video/spider/spider_api.cpp
  src/video/spider/spider_registry.cpp
  src/video/spider/spider_recipe.cpp
  src/video/spider/spider_http.cpp
  src/video/spider/spider_util.cpp
  src/video/spider/spider_builtins.cpp
  src/video/spider/spider_engine_cms.cpp
  src/video/spider/spider_engine_app.cpp
  src/video/spider/spider_engine_html.cpp
  src/video/spider/spider_engine_pan.cpp
  src/video/spider/spider_engine_live.cpp
  src/video/spider/spider_engine_recipe_api.cpp
)
# XP / Win7 兼容版：旧系统不能出现 ttplayer_video.dll 的启动导入，审计脚本也
# 禁止任何延迟导入。因此兼容版把视频层静态编进主 EXE（无 DLL / 无 delayload），
# 现代版保持独立 DLL + /DELAYLOAD 的「插件可选」语义不变。
if(TTPLAYER_LEGACY_WINDOWS)
  add_library(ttplayer_video STATIC ${TTPLAYER_VIDEO_SOURCES})
else()
  add_library(ttplayer_video SHARED ${TTPLAYER_VIDEO_SOURCES})
endif()
target_include_directories(ttplayer_video
  PUBLIC include
  PRIVATE "${TTPLAYER_GENERATED_INCLUDE_DIR}"
)
target_compile_definitions(ttplayer_video PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WTL_DPI_SUPPORT
  TTPLAYER_VIDEO_BUILDING_DLL)
target_compile_options(ttplayer_video PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(ttplayer_video PRIVATE
  ttplayer_core
  ttplayer_wtl_headers
  winmm ole32 oleaut32 comsuppw uuid strmiids msimg32 shell32 shlwapi comctl32
  comdlg32 windowscodecs gdiplus dsound setupapi dwmapi uxtheme mfplat mfreadwrite
  mf mfuuid propsys winhttp bcrypt)

add_library(ttpcomm_api STATIC src/app/ttpcomm_api.c)
target_include_directories(ttpcomm_api PUBLIC include)
target_compile_definitions(ttpcomm_api PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)

# 视频协议插件 DLL：每种流媒体协议一个独立 DLL，与音乐格式插件同约定，
# 部署到 <exe>\AddIn\ 并命名 ttp_v_<协议>.dll（工厂导出 ttpGetVideoProtocol）。
add_library(video_protocol_hls SHARED src/video/protocols/protocol_hls.cpp)
set_target_properties(video_protocol_hls PROPERTIES OUTPUT_NAME "ttp_v_hls")
target_include_directories(video_protocol_hls PUBLIC include)
target_compile_definitions(video_protocol_hls PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(video_protocol_hls PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(video_protocol_hls PRIVATE ws2_32 bcrypt)

# 网页播放地址协议：parses 解析接口 + 播放页内嵌直链 → 本地代理，
# 结果为 m3u8 时委托 ttp_v_hls.dll 摊平成 TS 流。
add_library(video_protocol_web SHARED src/video/protocols/protocol_web.cpp)
set_target_properties(video_protocol_web PROPERTIES OUTPUT_NAME "ttp_v_web")
target_include_directories(video_protocol_web PUBLIC include)
target_compile_definitions(video_protocol_web PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(video_protocol_web PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(video_protocol_web PRIVATE ws2_32 winhttp)

# MPEG-DASH：MPD → init + fMP4 分段的本地 MP4 代理（与 HLS 插件同骨架）。
add_library(video_protocol_dash SHARED src/video/protocols/protocol_dash.cpp)
set_target_properties(video_protocol_dash PROPERTIES OUTPUT_NAME "ttp_v_dash")
target_include_directories(video_protocol_dash PUBLIC include)
target_compile_definitions(video_protocol_dash PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(video_protocol_dash PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(video_protocol_dash PRIVATE ws2_32 bcrypt winhttp)

# 直播频道列表：M3U/TXT/JSON → 选频道 → 委托 HLS / WEB 协议插件。
add_library(video_protocol_m3u SHARED src/video/protocols/protocol_m3u.cpp)
set_target_properties(video_protocol_m3u PROPERTIES OUTPUT_NAME "ttp_v_m3u")
target_include_directories(video_protocol_m3u PUBLIC include)
target_compile_definitions(video_protocol_m3u PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(video_protocol_m3u PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(video_protocol_m3u PRIVATE ws2_32 winhttp)

# 直链媒体中转：mp4/flv/mkv… + 自定义请求头（Referer/Cookie）→ 本地 Range 代理。
# MF 窗口化引擎不能带请求头，B 站等防盗链 CDN 必须经由这层。
add_library(video_protocol_media SHARED src/video/protocols/protocol_media.cpp)
set_target_properties(video_protocol_media PROPERTIES OUTPUT_NAME "ttp_v_media")
target_include_directories(video_protocol_media PUBLIC include)
target_compile_definitions(video_protocol_media PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(video_protocol_media PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(video_protocol_media PRIVATE ws2_32 winhttp)

# ─────────────────────────────────────────────────────────────────────────────
# 内容插件（直播源 / 听书源 / TTS 引擎）：每种来源一个独立 DLL，
# 部署到 <exe>\AddIn\，命名 ttp_l_* / ttp_b_* / ttp_t_*。
# 宿主通过 LoadLibrary + GetProcAddress 加载（见 src/plugins/content_plugin_loader.h），
# 插件缺失只影响对应标签页。Win7 约束：插件只链接 ws2_32 / winhttp / ole32 等
# 系统库，不得引入 WebView2 或其它启动期依赖。
#
# ⚠ 插件实现（src/plugins/live/*、src/plugins/book/*、src/plugins/tts/*）尚未落地，
#   先保留 ABI 头与宿主注册表；插件源文件就绪后再取消下面四个目标的注释。
# add_library(live_source_m3u SHARED src/plugins/live/m3u_source.cpp)
# set_target_properties(live_source_m3u PROPERTIES OUTPUT_NAME "ttp_l_m3u")
# target_include_directories(live_source_m3u PRIVATE include)
# target_compile_definitions(live_source_m3u PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
# target_compile_options(live_source_m3u PRIVATE /W4 /permissive- /EHsc /utf-8)
# target_link_libraries(live_source_m3u PRIVATE ws2_32 winhttp)
#
# add_library(live_source_tvbox SHARED src/plugins/live/tvbox_source.cpp)
# set_target_properties(live_source_tvbox PROPERTIES OUTPUT_NAME "ttp_l_tvbox")
# target_include_directories(live_source_tvbox PRIVATE include)
# target_compile_definitions(live_source_tvbox PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
# target_compile_options(live_source_tvbox PRIVATE /W4 /permissive- /EHsc /utf-8)
# target_link_libraries(live_source_tvbox PRIVATE ws2_32 winhttp)
#
# add_library(book_source_radio SHARED src/plugins/book/radio_source.cpp)
# set_target_properties(book_source_radio PROPERTIES OUTPUT_NAME "ttp_b_radio")
# target_include_directories(book_source_radio PRIVATE include)
# target_compile_definitions(book_source_radio PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
# target_compile_options(book_source_radio PRIVATE /W4 /permissive- /EHsc /utf-8)
# target_link_libraries(book_source_radio PRIVATE ws2_32 winhttp)
#
# add_library(tts_provider_sapi SHARED src/plugins/tts/sapi_tts.cpp)
# set_target_properties(tts_provider_sapi PROPERTIES OUTPUT_NAME "ttp_t_sapi")
# target_include_directories(tts_provider_sapi PRIVATE include)
# target_compile_definitions(tts_provider_sapi PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
# target_compile_options(tts_provider_sapi PRIVATE /W4 /permissive- /EHsc /utf-8)
# target_link_libraries(tts_provider_sapi PRIVATE ole32 oleaut32)

# 内容插件联调探针：加载 AddIn 下的 ttp_l_/ttp_b_/ttp_t_ 插件并跑一遍。
add_executable(content_plugin_probe tools/content_plugin_probe.cpp)
target_include_directories(content_plugin_probe PRIVATE include)
target_compile_definitions(content_plugin_probe PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(content_plugin_probe PRIVATE /W3 /permissive- /EHsc /utf-8)
target_link_libraries(content_plugin_probe PRIVATE ttplayer_core)

# 视频协议插件全链路自测：加载 ttp_v_hls / ttp_v_web，打开会话、轮询就绪、
# 抓取最终媒体地址（TS 本地流）的字节并校验 MPEG-TS 同步字节。
add_executable(selftest_video tools/selftest_video.cpp)
target_include_directories(selftest_video PRIVATE include)
target_compile_definitions(selftest_video PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(selftest_video PRIVATE /W3 /permissive- /EHsc /utf-8)
target_link_libraries(selftest_video PRIVATE ws2_32 winhttp)

# 音乐窗搜索无 GUI 探针：直接调用 musicfree::SearchAllSources，
# 打印音源数 / 命中数 / 每音源错误原因。放在 TTPlayer\ 下运行。
add_executable(music_search_probe tools/music_search_probe.cpp)
target_include_directories(music_search_probe PRIVATE include)
target_compile_definitions(music_search_probe PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(music_search_probe PRIVATE /W3 /permissive- /EHsc /utf-8)
target_link_libraries(music_search_probe PRIVATE ttplayer_core)

# 按平台逐个取链的探针：定位「多个源都播不了」是普遍失败还是个别源问题。
add_executable(music_resolve_probe tools/music_resolve_probe.cpp)
target_include_directories(music_resolve_probe PRIVATE include)
target_compile_definitions(music_resolve_probe PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(music_resolve_probe PRIVATE /W3 /permissive- /EHsc /utf-8)
target_link_libraries(music_resolve_probe PRIVATE ttplayer_core)

# 视频全源搜索探针：与 video.search 同一实现（8 路并发 + 单站计时）。
add_executable(video_search_probe tools/video_search_probe.cpp)
target_include_directories(video_search_probe PRIVATE include)
target_compile_definitions(video_search_probe PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(video_search_probe PRIVATE /W3 /permissive- /EHsc /utf-8)
target_link_libraries(video_search_probe PRIVATE ttplayer_core ttplayer_video)

# 图片解码插件自测：加载 ttp_i_webp.dll，解码真实缓存封面并校验/导出 BMP。
add_executable(selftest_image tools/selftest_image.cpp)
target_include_directories(selftest_image PRIVATE include)
target_compile_definitions(selftest_image PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(selftest_image PRIVATE /W3 /permissive- /EHsc /utf-8)
target_link_libraries(selftest_image PRIVATE gdi32)

# 原生 spider 自检（SubTask 1.7）：注册表 / 分派矩阵（普通 CMS、直播、drpy、
# 已实现与未实现的 csp_*）/ recipe 目录探测与校验。无 GUI，输出
# SPIDER_PROBE_RESULT: PASS|FAIL 并以 0/1 退出。
add_executable(spider_probe tools/spider_probe.cpp)
target_include_directories(spider_probe PRIVATE include)
target_compile_definitions(spider_probe PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(spider_probe PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(spider_probe PRIVATE ttplayer_core)

# 原生 spider 全量自测（Task 8.1）：单站点四段（home/category/detail/player）
# 与 --all 遍历 sites_resolved.json；--play 时校验 HLS/FLV/MP4 真实字节，可选
# 转调同目录 selftest_video.exe 做插件级校验。报告 UTF-8，末行
# SELFTEST_SPIDER_RESULT: PASS|FAIL 并以 0/1 退出。
add_executable(selftest_spider tools/selftest_spider.cpp)
target_include_directories(selftest_spider PRIVATE include)
target_compile_definitions(selftest_spider PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(selftest_spider PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(selftest_spider PRIVATE ttplayer_core)

# TVBox/catvod/drpy 自定义播放器宿主内部自测（纯本地内联 JS，无网络）：
# 校验 playerContent / flag 命名函数 / async 入口、宿主 API、请求头回传。
add_executable(selftest_player_js tools/selftest_player_js.cpp)
target_include_directories(selftest_player_js PRIVATE include)
target_compile_definitions(selftest_player_js PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(selftest_player_js PRIVATE /W3 /permissive- /EHsc /utf-8)
target_link_libraries(selftest_player_js PRIVATE ttplayer_video ttplayer_core)

# 图片解码插件：WebP（含 lossy VP8 / lossless VP8L / 透明 / 动画首帧）。
# 与音乐/视频插件同约定：位置 <exe>\AddIn\，命名 ttp_i_<格式>.dll，工厂导出
# ttpGetImageDecoder。解码核心由 libwebp（BSD）静态编入本 DLL 完成 —— 零外部运行时，
# 不依赖系统 WebP 解码器，跨 Win10/11 与精简版系统都能解出封面。
# 直接复用 libwebp 自带的 CMake（它按架构/编译选项条件编译 SIMD，避免手工拣源出错），
# 仅启用解码链路：关闭全部可执行文件与 mux/extras，只暴露 webpdecode/webpdspdecode/
# webputilsdecode 三个 object 库供本插件链接。
set(WEBP_BUILD_LIBWEBPMUX OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_ANIM_UTILS OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_CWEBP       OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_DWEBP       OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_GIF2WEBP    OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_IMG2WEBP    OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_VWEBP       OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPINFO    OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPMUX     OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_EXTRAS      OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBP_JS     OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/libwebp-1.4.0)

add_library(image_decoder_webp SHARED src/plugins/image/decoder_webp.cpp)
set_target_properties(image_decoder_webp PROPERTIES OUTPUT_NAME "ttp_i_webp")
target_include_directories(image_decoder_webp PRIVATE include third_party/libwebp-1.4.0/src)
target_compile_definitions(image_decoder_webp PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(image_decoder_webp PRIVATE /W3 /permissive- /EHsc /utf-8)
target_link_libraries(image_decoder_webp PRIVATE
  webpdecode webpdspdecode webputilsdecode gdi32 kernel32 user32)

add_executable(ttplayer_rebuild WIN32
  src/app/application.cpp
  src/app/exports.cpp
  src/app/main.cpp
  src/app/resources.rc
  src/app/runtime.cpp
  src/app/single_instance.cpp
  src/app/ttplayer.manifest
)
set_target_properties(ttplayer_rebuild PROPERTIES
  OUTPUT_NAME "TTPlayerRebuild" PDB_NAME "TTPlayerRebuild")
target_compile_definitions(ttplayer_rebuild PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(ttplayer_rebuild PRIVATE /W4 /permissive- /EHsc /utf-8)
# ttplayer_core 在前、ttplayer_video 在后：core 静态库里 musicfree 桥接代码
# 引用的视频符号由 video 的导入库补齐（MSVC 按序搜索库）。
# /DELAYLOAD + delayimp：ttplayer_video.dll 延迟到首次视频调用才解析 ——
# DLL 缺失时主程序照常启动（视频模块缺失走 main.cpp 的失败钩子兜底提示），
# 与「插件可选」语义一致。
if(TTPLAYER_LEGACY_WINDOWS)
  # 静态库：不能有 delayimp / 延迟导入，也不必复制 DLL。
  target_link_libraries(ttplayer_rebuild PRIVATE
    ttplayer_core ttplayer_video ttpcomm_api comctl32 comdlg32 shell32)
else()
  target_link_libraries(ttplayer_rebuild PRIVATE
    ttplayer_core ttplayer_video ttpcomm_api comctl32 comdlg32 shell32 delayimp)
  target_link_options(ttplayer_rebuild PRIVATE /DELAYLOAD:ttplayer_video.dll)
  add_custom_command(TARGET ttplayer_rebuild POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:ttplayer_video>"
            "$<TARGET_FILE_DIR:ttplayer_rebuild>")
endif()
add_dependencies(ttplayer_rebuild ttplayer_video)

# ─────────────────── WebView2 SDK 接线（Win32/x86，本地 SDK 无网络）───────────────────
# 三行照 ../../third_party/webview2/README.md 的要求：头文件目录 + x86 库搜索路径 +
# 导入库 WebView2Loader.dll.lib（README 明确导入库/静态库二选一，取导入库，
# 因此运行时必须带 WebView2Loader.dll —— 见文件末尾的 post-build 复制）。
# 只用 build/native/x86，不破坏文件头的 32 位约束；不引入 FetchContent/下载。
target_include_directories(ttplayer_rebuild PRIVATE
  ../../third_party/webview2/build/native/include
)
target_link_directories(ttplayer_rebuild PRIVATE
  ../../third_party/webview2/build/native/x86
)
# XP / Win7 版不链接导入库：启动导入表里出现 WebView2Loader.dll 会让旧系统
# 加载器直接拒绝启动。改为运行时 LoadLibrary（见 webview2_dynamic.h），
# 旧系统没有 WebView2 Runtime 时优雅回退原生界面。
if(NOT TTPLAYER_LEGACY_WINDOWS)
  target_link_libraries(ttplayer_rebuild PRIVATE WebView2Loader.dll.lib)
endif()

# 音乐窗 WebView2 化的三个新增源文件（编入主 exe）：
#   musicfree_webview_host.cpp —— WebView2 宿主（并行任务创建，此处按文件名注册）
#   webui_assets.cpp           —— 页面资产双源加载（外部覆盖优先 + 嵌资源兜底）
#   musicfree_bridge.cpp       —— C++ ↔ 页面 JSON 双向桥协议层
# 三个能力域的命令注册入口（编入主 exe，由音乐窗 ShowMusicFreeBrowser 建桥后各调一次）：
#   bridge_cmds_music.cpp      —— 音乐能力命令（RegisterMusicCommands，并行任务创建）
#   bridge_cmds_video.cpp      —— 视频能力命令（RegisterVideoCommands）
#   bridge_cmds_misc.cpp       —— 搜索/收藏/历史/配置命令（RegisterMiscCommands）
target_sources(ttplayer_rebuild PRIVATE
  src/musicfree/musicfree_webview_host.cpp
  src/musicfree/webui_assets.cpp
  src/musicfree/musicfree_bridge.cpp
  src/musicfree/bridge_cmds_music.cpp
  src/musicfree/bridge_cmds_video.cpp
  src/musicfree/bridge_cmds_misc.cpp
  # 视频窗网页兜底宿主（WebView2）：官方网页线路解析全失败时在播放窗内加载网页。
  # 只编入 exe：核心库经 ttplayer/video/video_web_host.h 的函数指针调用它。
  src/musicfree/video_web_host.cpp
)

# WebUI 三件套是被 resources.rc 以 RCDATA 直接引用的；MSVC 的 RC 依赖文件
# 不会自动跟踪这些 include，改页面后 ninja 会误判“资源无需重编”，导致 exe
# 里仍是旧页面。这里显式加 OBJECT_DEPENDS，页面一变就重编资源并重链接。
set_property(SOURCE src/app/resources.rc APPEND PROPERTY OBJECT_DEPENDS
  "${CMAKE_SOURCE_DIR}/../../webui/index.html;${CMAKE_SOURCE_DIR}/../../webui/style.css;${CMAKE_SOURCE_DIR}/../../webui/app.js")

# 崩溃定位用链接映射表：发布版没有 PDB，线上 dump 只能靠 map 把「exe+RVA」
# 还原成函数名。map 不改变代码布局，可长期保留。
if(MSVC)
  target_link_options(ttplayer_rebuild PRIVATE
    "/MAP:${CMAKE_BINARY_DIR}/TTPlayerRebuild.map")
endif()

# 视频网页兜底宿主的离屏冒烟探针（验证用，不进产品运行时依赖）。
# 隐藏父窗口 + 真实腾讯播放页，20 秒消息循环内判定 READY/FAILED，
# 末行输出 WEBVIEW_PROBE_RESULT: PASS|FAIL 并以 0/1 退出。
if(NOT TTPLAYER_LEGACY_WINDOWS)
  add_executable(webview_probe
    tools/webview_probe.cpp
    src/musicfree/video_web_host.cpp
  )
  target_include_directories(webview_probe PRIVATE
    include
    ../../third_party/webview2/build/native/include
  )
  target_link_directories(webview_probe PRIVATE
    ../../third_party/webview2/build/native/x86
  )
  target_compile_definitions(webview_probe PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(webview_probe PRIVATE /W4 /permissive- /EHsc /utf-8)
  target_link_libraries(webview_probe PRIVATE
    ttplayer_core WebView2Loader.dll.lib ole32 oleaut32 user32)

  # WebView2 运行时选择（Win10+ 系统 Runtime / Win7 109 / 自动安装开关）的内部自测。
  add_executable(selftest_webview2_runtime tools/selftest_webview2_runtime.cpp)
  target_include_directories(selftest_webview2_runtime PRIVATE include)
  target_compile_definitions(selftest_webview2_runtime PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(selftest_webview2_runtime PRIVATE /W3 /permissive- /EHsc /utf-8)
  target_link_libraries(selftest_webview2_runtime PRIVATE ttplayer_core)
endif()

# The player and translation provider are built and distributed independently.
# An already-built DLL is only needed for optional integration tests.
set(TTPLAYER_I18N_TEST_DLL "" CACHE FILEPATH "Prebuilt ttp_i18n.dll for local integration tests")
if(BUILD_TESTING AND TTPLAYER_I18N_TEST_DLL)
  if(NOT EXISTS "${TTPLAYER_I18N_TEST_DLL}" OR IS_DIRECTORY "${TTPLAYER_I18N_TEST_DLL}")
    message(FATAL_ERROR "TTPLAYER_I18N_TEST_DLL must point to an independently built ttp_i18n.dll")
  endif()
  add_executable(i18n_ui_tests tests/ui/i18n_tests.cpp tests/ui/i18n_test_resources.rc src/app/resources.rc)
  target_compile_definitions(i18n_ui_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(i18n_ui_tests PRIVATE /W4 /EHsc /utf-8)
  target_link_libraries(i18n_ui_tests PRIVATE ttplayer_core)
  foreach(language IN ITEMS chs cht)
    add_library(i18n_resource_mock_${language} SHARED tests/ui/i18n_mock_resources.rc)
    set_target_properties(i18n_resource_mock_${language} PROPERTIES LINKER_LANGUAGE CXX)
    target_link_options(i18n_resource_mock_${language} PRIVATE /NOENTRY)
    if(language STREQUAL "cht")
      target_compile_definitions(i18n_resource_mock_${language} PRIVATE I18N_TEST_TRADITIONAL)
    endif()
    add_dependencies(i18n_ui_tests i18n_resource_mock_${language})
  endforeach()
  add_test(NAME i18n_ui_tests COMMAND i18n_ui_tests "${TTPLAYER_I18N_TEST_DLL}"
    "$<TARGET_FILE:i18n_resource_mock_chs>" "$<TARGET_FILE:i18n_resource_mock_cht>")
  if(NOT TTPLAYER_LEGACY_WINDOWS)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_dependencies(i18n_ui_tests ttplayer_rebuild ttpcomm_mock_no_version)
    add_test(NAME i18n_startup_tests COMMAND "${Python3_EXECUTABLE}" -X utf8
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/ui/i18n_startup_tests.py"
      "$<TARGET_FILE:ttplayer_rebuild>" "${TTPLAYER_I18N_TEST_DLL}"
      "$<TARGET_FILE:ttpcomm_mock_no_version>")
    set_tests_properties(i18n_startup_tests PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
  endif()
endif()

include(cmake/size_optimization.cmake)

# Pre-ATL-8 AddIns generate window thunks in ordinary heap memory
# (ttp_ape!60101C36, also ttp_enc). /NXCOMPAT opts out of Windows' ATL
# emulation, faulting before WM_INITDIALOG finishes. wWinMain instead
# enables DEP explicitly, with the system's legacy ATL compatibility.
target_link_options(ttplayer_rebuild PRIVATE /NXCOMPAT:NO)
# 崩溃诊断：生成 public symbol 映射，配合 %TEMP%\ttplayer_crash.log 的 RVA 定位函数
target_link_options(ttplayer_rebuild PRIVATE
  /MAP:$<TARGET_FILE_DIR:ttplayer_rebuild>/ttplayer_rebuild.map)
if(TTPLAYER_LEGACY_WINDOWS)
  include(cmake/legacy_ttpcomm.cmake)
  target_link_options(ttplayer_rebuild PRIVATE /SUBSYSTEM:WINDOWS,5.01 /OSVERSION:5.1)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_custom_command(TARGET ttplayer_rebuild POST_BUILD
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_legacy_imports.py"
      "$<TARGET_FILE:ttplayer_rebuild>"
      --exports "${ttplayer_yy_thunks_SOURCE_DIR}/Config/x86/5.1.2600.txt"
      --exports "${ttplayer_yy_thunks_SOURCE_DIR}/Config/x86/6.1.7600.txt"
      --require-ttpcomm-tls
      --report "$<TARGET_FILE_DIR:ttplayer_rebuild>/legacy-imports.json"
    COMMENT "Checking every static import against XP and Windows 7"
    VERBATIM)
  set_property(TARGET ttplayer_rebuild APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_legacy_imports.py")
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(TTPLAYER_LAME_ARCH x64)
else()
  set(TTPLAYER_LAME_ARCH x86)
endif()
function(ttplayer_stage_lame target)
  set(lame_archive "${CMAKE_CURRENT_SOURCE_DIR}/../lame-4.0-${TTPLAYER_LAME_ARCH}.zip")
  set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/stage_lame.cmake")
  if(EXISTS "${lame_archive}")
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${lame_archive}")
  endif()
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
      "-DARCHIVE=${CMAKE_CURRENT_SOURCE_DIR}/../lame-4.0-${TTPLAYER_LAME_ARCH}.zip"
      "-DSTAGING=${CMAKE_CURRENT_BINARY_DIR}/lame/${TTPLAYER_LAME_ARCH}"
      "-DDESTINATION=$<TARGET_FILE_DIR:${target}>"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/stage_lame.cmake"
    VERBATIM)
endfunction()
if(TTPLAYER_STAGE_RUNTIME)
  ttplayer_stage_lame(ttplayer_rebuild)
endif()

# Standalone development/test front ends only. The shipped player embeds all
# three workers and relaunches itself in private modes for fault isolation;
# building/distributing ttplayer_rebuild does not require any helper EXE.
if(BUILD_TESTING OR TTPLAYER_BUILD_PREVIEW_TESTS)
enable_testing()
add_executable(taskbar_preview_tests tests/ui/taskbar_preview_tests.cpp)
target_compile_definitions(taskbar_preview_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(taskbar_preview_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(taskbar_preview_tests PRIVATE ttplayer_core)
add_test(NAME taskbar_preview_tests COMMAND taskbar_preview_tests)
set_tests_properties(taskbar_preview_tests PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
endif()

if(BUILD_TESTING OR TTPLAYER_BUILD_WHEEL_TESTS)
enable_testing()
add_executable(playlist_wheel_tests tests/ui/playlist_wheel_tests.cpp)
target_compile_definitions(playlist_wheel_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(playlist_wheel_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(playlist_wheel_tests PRIVATE ttplayer_core)
add_test(NAME playlist_wheel_tests COMMAND playlist_wheel_tests)
set_tests_properties(playlist_wheel_tests PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
endif()

if(BUILD_TESTING OR TTPLAYER_BUILD_INFO_TESTS)
enable_testing()
add_executable(playlist_info_tests tests/ui/playlist_info_tests.cpp)
target_compile_definitions(playlist_info_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(playlist_info_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(playlist_info_tests PRIVATE ttplayer_core)
add_test(NAME playlist_info_tests COMMAND playlist_info_tests)
set_tests_properties(playlist_info_tests PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
endif()

if(BUILD_TESTING OR TTPLAYER_BUILD_NAVIGATION_TESTS)
enable_testing()
add_executable(playback_navigation_tests tests/ui/playback_navigation_tests.cpp)
target_compile_definitions(playback_navigation_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(playback_navigation_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(playback_navigation_tests PRIVATE ttplayer_core)
add_test(NAME playback_navigation_tests COMMAND playback_navigation_tests)
set_tests_properties(playback_navigation_tests PROPERTIES TIMEOUT 30)
endif()

if(BUILD_TESTING OR TTPLAYER_BUILD_LIBRARY_TESTS)
enable_testing()
add_executable(media_library_tests tests/ui/media_library_tests.cpp
  src/app/resources.rc src/app/ttplayer.manifest)
target_compile_definitions(media_library_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(media_library_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(media_library_tests PRIVATE ttplayer_core)
add_test(NAME media_library_tests COMMAND media_library_tests "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_tests_properties(media_library_tests PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
endif()

if(BUILD_TESTING)
add_executable(track_change_fade_tests tests/ui/track_change_fade_tests.cpp)
target_compile_definitions(track_change_fade_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(track_change_fade_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(track_change_fade_tests PRIVATE ttplayer_core)
add_dependencies(track_change_fade_tests dsp_probe_valid)
add_test(NAME track_change_fade_tests COMMAND track_change_fade_tests)
set_tests_properties(track_change_fade_tests PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
add_executable(midi_playback_tests tests/audio/midi_playback_tests.cpp)
target_compile_definitions(midi_playback_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(midi_playback_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(midi_playback_tests PRIVATE ttplayer_core)
add_test(NAME midi_playback_tests COMMAND midi_playback_tests)
set_tests_properties(midi_playback_tests PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
add_executable(cover_format_tests tests/ui/cover_format_tests.cpp src/app/exports.cpp)
target_compile_definitions(cover_format_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(cover_format_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(cover_format_tests PRIVATE ttplayer_core)
add_test(NAME cover_format_tests COMMAND cover_format_tests
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/album_cover" "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_tests_properties(cover_format_tests PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
if(TTPLAYER_LEGACY_WINDOWS)
  add_dependencies(cover_format_tests ttplayer_legacy_ttpcomm_import)
  target_link_libraries(cover_format_tests PRIVATE "${ttpcomm_import_library}")
  target_link_options(cover_format_tests PRIVATE /INCLUDE:__imp__srand48)
endif()
add_executable(cover_image_tests tests/ui/cover_image_tests.cpp)
target_compile_definitions(cover_image_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(cover_image_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(cover_image_tests PRIVATE ttplayer_core)
if(TTPLAYER_LEGACY_WINDOWS)
  add_dependencies(cover_image_tests ttplayer_legacy_ttpcomm_import)
  target_link_libraries(cover_image_tests PRIVATE "${ttpcomm_import_library}")
  target_link_options(cover_image_tests PRIVATE /INCLUDE:__imp__srand48)
endif()
add_test(NAME cover_image_tests COMMAND cover_image_tests
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/album_cover" "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_tests_properties(cover_image_tests PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)

add_executable(idle_error_tests tests/ui/idle_error_tests.cpp
  src/app/resources.rc src/app/ttplayer.manifest)
target_compile_definitions(idle_error_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(idle_error_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(idle_error_tests PRIVATE ttplayer_core)
add_test(NAME idle_error_tests COMMAND idle_error_tests "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_tests_properties(idle_error_tests PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)

add_executable(default_colors_tests tests/skin/default_colors_tests.cpp)
target_compile_definitions(default_colors_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(default_colors_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(default_colors_tests PRIVATE ttplayer_core)
add_test(NAME default_colors_tests COMMAND default_colors_tests "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_tests_properties(default_colors_tests PROPERTIES TIMEOUT 100 RUN_SERIAL TRUE)

add_executable(legacy_windows_tests tests/platform/legacy_windows_tests.cpp
  src/platform/optional_windows_api.cpp)
target_compile_definitions(legacy_windows_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN TTPLAYER_TEST_NO_OPTIONAL_WINDOWS_API)
target_compile_options(legacy_windows_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(legacy_windows_tests PRIVATE ttplayer_core)
add_test(NAME legacy_windows_tests COMMAND legacy_windows_tests)
set_tests_properties(legacy_windows_tests PROPERTIES TIMEOUT 45)

add_executable(runtime_paths_tests tests/ui/runtime_paths_tests.cpp
  src/app/resources.rc src/app/ttplayer.manifest)
target_compile_definitions(runtime_paths_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(runtime_paths_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(runtime_paths_tests PRIVATE ttplayer_core)
add_dependencies(runtime_paths_tests ttplayer_rebuild)
add_test(NAME runtime_paths_tests COMMAND runtime_paths_tests "${CMAKE_CURRENT_SOURCE_DIR}/.."
  "$<TARGET_FILE:ttplayer_rebuild>")
set_tests_properties(runtime_paths_tests PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
add_test(NAME stage_settings_tests COMMAND "${CMAKE_COMMAND}"
  "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/Testing"
  -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_stage_settings.cmake")

add_executable(fullscreen_lyric_drag_tests tests/ui/fullscreen_lyric_drag_tests.cpp
  src/app/resources.rc src/app/ttplayer.manifest)
target_compile_definitions(fullscreen_lyric_drag_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(fullscreen_lyric_drag_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(fullscreen_lyric_drag_tests PRIVATE ttplayer_core)
add_test(NAME fullscreen_lyric_drag_tests COMMAND fullscreen_lyric_drag_tests "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_tests_properties(fullscreen_lyric_drag_tests PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)

add_executable(fullscreen_lyric_render_tests tests/ui/fullscreen_lyric_render_tests.cpp
  src/app/resources.rc src/app/ttplayer.manifest)
target_compile_definitions(fullscreen_lyric_render_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(fullscreen_lyric_render_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(fullscreen_lyric_render_tests PRIVATE ttplayer_core)
add_test(NAME fullscreen_lyric_render_tests COMMAND fullscreen_lyric_render_tests "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_tests_properties(fullscreen_lyric_render_tests PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)

add_executable(fullscreen_album_tests tests/ui/fullscreen_album_tests.cpp
  src/app/resources.rc src/app/ttplayer.manifest)
target_compile_definitions(fullscreen_album_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(fullscreen_album_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(fullscreen_album_tests PRIVATE ttplayer_core)
add_test(NAME fullscreen_album_tests COMMAND fullscreen_album_tests "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_tests_properties(fullscreen_album_tests PROPERTIES TIMEOUT 45)

add_executable(desktop_lyrics_menu_tests tests/ui/desktop_lyrics_menu_tests.cpp
  src/app/resources.rc src/app/ttplayer.manifest)
target_compile_definitions(desktop_lyrics_menu_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(desktop_lyrics_menu_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(desktop_lyrics_menu_tests PRIVATE ttplayer_core)
add_test(NAME desktop_lyrics_menu_tests COMMAND desktop_lyrics_menu_tests "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_tests_properties(desktop_lyrics_menu_tests PROPERTIES TIMEOUT 45 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)

add_executable(options_drawing_tests tests/ui/options_drawing_tests.cpp
  src/app/resources.rc src/app/ttplayer.manifest)
target_compile_definitions(options_drawing_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(options_drawing_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(options_drawing_tests PRIVATE ttplayer_core)
add_test(NAME options_drawing_tests COMMAND options_drawing_tests "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_tests_properties(options_drawing_tests PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
add_test(NAME options_general_tabs_tests COMMAND options_drawing_tests "${CMAKE_CURRENT_SOURCE_DIR}/.." --general-tabs)
set_tests_properties(options_general_tabs_tests PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)

add_executable(lyric_search_tests tests/ui/lyric_search_tests.cpp src/app/resources.rc src/app/ttplayer.manifest)
target_compile_definitions(lyric_search_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(lyric_search_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(lyric_search_tests PRIVATE ttplayer_core ws2_32)
add_test(NAME lyric_search_tests COMMAND lyric_search_tests "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_tests_properties(lyric_search_tests PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)

add_executable(lyric_menu_tests tests/ui/lyric_menu_tests.cpp src/app/resources.rc src/app/ttplayer.manifest)
target_compile_definitions(lyric_menu_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(lyric_menu_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(lyric_menu_tests PRIVATE ttplayer_core oleacc)
add_test(NAME lyric_menu_tests COMMAND lyric_menu_tests "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_tests_properties(lyric_menu_tests PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)

add_executable(lyric_association_tests tests/ui/lyric_association_tests.cpp
  src/app/resources.rc src/app/ttplayer.manifest)
target_compile_definitions(lyric_association_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(lyric_association_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(lyric_association_tests PRIVATE ttplayer_core)
add_test(NAME lyric_association_tests COMMAND lyric_association_tests "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_tests_properties(lyric_association_tests PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)

add_executable(lyric_service_catalog_tests tests/lyrics/service_catalog_tests.cpp)
target_compile_definitions(lyric_service_catalog_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(lyric_service_catalog_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(lyric_service_catalog_tests PRIVATE ttplayer_core)
add_test(NAME lyric_service_catalog_tests COMMAND lyric_service_catalog_tests "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_tests_properties(lyric_service_catalog_tests PROPERTIES TIMEOUT 60)

add_executable(window_topmost_tests tests/ui/window_topmost_tests.cpp
  src/app/resources.rc src/app/ttplayer.manifest)
target_compile_definitions(window_topmost_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(window_topmost_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(window_topmost_tests PRIVATE ttplayer_core)
add_test(NAME window_topmost_tests COMMAND window_topmost_tests "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_tests_properties(window_topmost_tests PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)

add_executable(skin_profile_tests tests/skin/skin_profile_tests.cpp)
target_compile_definitions(skin_profile_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(skin_profile_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(skin_profile_tests PRIVATE ttplayer_core)
add_test(NAME skin_profile_tests COMMAND skin_profile_tests "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_tests_properties(skin_profile_tests PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)

add_executable(skin_alignment_tests tests/skin/skin_alignment_tests.cpp)
target_compile_definitions(skin_alignment_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(skin_alignment_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(skin_alignment_tests PRIVATE ttplayer_core)
add_test(NAME skin_alignment_tests COMMAND skin_alignment_tests "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_executable(project_links_tests tests/ui/project_links_tests.cpp
  src/app/resources.rc src/app/ttplayer.manifest)
target_compile_definitions(project_links_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(project_links_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(project_links_tests PRIVATE ttplayer_core)
add_test(NAME project_links_tests COMMAND project_links_tests "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_executable(wtl_behavior_tests tests/ui/wtl_behavior_tests.cpp
  src/app/resources.rc src/app/ttplayer.manifest)
target_compile_definitions(wtl_behavior_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(wtl_behavior_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(wtl_behavior_tests PRIVATE ttplayer_core ttpcomm_api)
add_test(NAME wtl_behavior_tests COMMAND wtl_behavior_tests)
set_tests_properties(wtl_behavior_tests PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)

add_executable(wtl_migration_tests tests/ui/wtl_migration_tests.cpp)
target_compile_definitions(wtl_migration_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(wtl_migration_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(wtl_migration_tests PRIVATE ttplayer_core)
add_test(NAME wtl_migration_tests COMMAND wtl_migration_tests)
set_tests_properties(wtl_migration_tests PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)

add_executable(skin_png_tests tests/skin/skin_png_tests.cpp)
target_compile_definitions(skin_png_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(skin_png_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(skin_png_tests PRIVATE ttplayer_core)
add_test(NAME skin_png_tests COMMAND skin_png_tests "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_executable(ttplayer_dsp_probe EXCLUDE_FROM_ALL
  tools/dsp_plugin_probe.cpp src/app/dsp_worker.cpp)
target_include_directories(ttplayer_dsp_probe PRIVATE include)
target_compile_definitions(ttplayer_dsp_probe PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(ttplayer_dsp_probe PRIVATE /W4 /permissive- /EHsc /utf-8)
set_property(TARGET ttplayer_dsp_probe PROPERTY MSVC_RUNTIME_LIBRARY MultiThreaded)

# Legacy output discovery enters SetupAPI, KS miniports and DirectSound driver
# code.  Keep the original catalogue/order, but run that blocking boundary in
# a same-bitness helper which the modeless Options window can time out safely.
add_executable(ttplayer_output_device_probe EXCLUDE_FROM_ALL
  tools/output_device_probe.cpp
  src/app/output_device_worker.cpp
  src/audio/asio_sink.cpp
  src/audio/native_output_contract.cpp
  src/ui/output_devices.cpp
)
target_include_directories(ttplayer_output_device_probe PRIVATE src/ui include)
target_compile_definitions(ttplayer_output_device_probe PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(ttplayer_output_device_probe PRIVATE
  /W4 /permissive- /EHsc /utf-8)
target_link_libraries(ttplayer_output_device_probe PRIVATE
  winmm dsound setupapi ole32)
set_property(TARGET ttplayer_output_device_probe PROPERTY
  MSVC_RUNTIME_LIBRARY MultiThreaded)

# CFileInfoDlg enters third-party reader/tag-writer vtables on worker threads.
# Keep that recovered ABI, but isolate it so Close/cancel can enforce a hard
# bound even when an old add-in never returns from Open or metadata slot 6.
add_executable(ttplayer_file_info_probe EXCLUDE_FROM_ALL tools/file_info_probe.cpp)
target_include_directories(ttplayer_file_info_probe PRIVATE src/ui)
target_compile_definitions(ttplayer_file_info_probe PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(ttplayer_file_info_probe PRIVATE
  /W4 /permissive- /EHsc /utf-8)
target_link_libraries(ttplayer_file_info_probe PRIVATE ttplayer_core)

foreach(dsp_fixture IN ITEMS valid old stall fault pacemaker configure)
  add_library(dsp_probe_${dsp_fixture} SHARED tests/mock_winamp_dsp.cpp)
  target_compile_definitions(dsp_probe_${dsp_fixture} PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(dsp_probe_${dsp_fixture} PRIVATE
    /W4 /permissive- /EHsc /utf-8)
  set_property(TARGET dsp_probe_${dsp_fixture} PROPERTY
    MSVC_RUNTIME_LIBRARY MultiThreaded)
  set_target_properties(dsp_probe_${dsp_fixture} PROPERTIES
    PREFIX "" OUTPUT_NAME "dsp_${dsp_fixture}")
endforeach()
target_compile_definitions(dsp_probe_old PRIVATE TTPLAYER_DSP_OLD_HEADER)
target_compile_definitions(dsp_probe_stall PRIVATE TTPLAYER_DSP_STALL_HEADER)
target_compile_definitions(dsp_probe_fault PRIVATE TTPLAYER_DSP_FAULT_MODIFY)
target_compile_definitions(dsp_probe_pacemaker PRIVATE
  TTPLAYER_DSP_PACEMAKER_HEADER)
target_compile_definitions(dsp_probe_configure PRIVATE TTPLAYER_DSP_CONFIGURE_HEADER)

add_executable(portable_worker_tests tests/portable_worker_tests.cpp)
target_include_directories(portable_worker_tests PRIVATE src/ui)
target_compile_definitions(portable_worker_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(portable_worker_tests PRIVATE /W4 /permissive- /EHsc /utf-8 /UNDEBUG)
target_link_libraries(portable_worker_tests PRIVATE ttplayer_core)
add_dependencies(portable_worker_tests ttplayer_rebuild dsp_probe_configure
  ttplayer_output_device_probe)

add_executable(dsp_probe_tests tests/dsp_probe_tests.cpp)
target_compile_definitions(dsp_probe_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(dsp_probe_tests PRIVATE
  /W4 /permissive- /EHsc /utf-8 /UNDEBUG)
set_property(TARGET dsp_probe_tests PROPERTY MSVC_RUNTIME_LIBRARY MultiThreaded)
add_dependencies(dsp_probe_tests ttplayer_dsp_probe dsp_probe_valid
  dsp_probe_old dsp_probe_stall)

add_executable(winamp_dsp_runtime_tests tests/winamp_dsp_runtime_tests.cpp)
target_compile_definitions(winamp_dsp_runtime_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(winamp_dsp_runtime_tests PRIVATE
  /W4 /permissive- /EHsc /utf-8 /UNDEBUG)
target_link_libraries(winamp_dsp_runtime_tests PRIVATE ttplayer_core)
add_dependencies(winamp_dsp_runtime_tests dsp_probe_valid dsp_probe_old
  dsp_probe_fault dsp_probe_pacemaker)

add_executable(ttplayer_tests tests/tests.cpp)
target_compile_definitions(ttplayer_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
# This executable is itself a regression oracle.  MSVC's Release defaults add
# NDEBUG, which used to compile all 300+ assert expressions out of the guest
# Sandbox run while still returning exit code zero.  Keep those checks live in
# every configuration; the product targets retain their normal Release flags.
target_compile_options(ttplayer_tests PRIVATE
  /W4 /permissive- /EHsc /utf-8 /UNDEBUG)
target_link_libraries(ttplayer_tests PRIVATE ttplayer_core)

add_executable(file_association_tests tests/file_association_tests.cpp)
add_executable(default_programs_tests tests/settings/default_programs_tests.cpp)
target_compile_definitions(default_programs_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(default_programs_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(default_programs_tests PRIVATE ttplayer_core)
add_test(NAME default_programs_tests COMMAND default_programs_tests)
set_tests_properties(default_programs_tests PROPERTIES TIMEOUT 30)
target_compile_definitions(file_association_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(file_association_tests PRIVATE
  /W4 /permissive- /EHsc /utf-8 /UNDEBUG)
target_link_libraries(file_association_tests PRIVATE ttplayer_core)

add_executable(desktop_lyrics_tests tests/desktop_lyrics_tests.cpp
  tests/desktop_lyric_scroll_tests.cpp)
target_compile_definitions(desktop_lyrics_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(desktop_lyrics_tests PRIVATE
  /W4 /permissive- /EHsc /utf-8 /UNDEBUG)
target_link_libraries(desktop_lyrics_tests PRIVATE ttplayer_core)

add_executable(shell_send_to_tests tests/shell_send_to_tests.cpp)
target_compile_definitions(shell_send_to_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(shell_send_to_tests PRIVATE
  /W4 /permissive- /EHsc /utf-8 /UNDEBUG)
target_link_libraries(shell_send_to_tests PRIVATE ttplayer_core)

add_executable(playlist_network_commands_tests
  tests/playlist_network_commands_tests.cpp)
target_compile_definitions(playlist_network_commands_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(playlist_network_commands_tests PRIVATE
  /W4 /permissive- /EHsc /utf-8 /UNDEBUG)
target_link_libraries(playlist_network_commands_tests PRIVATE ttplayer_core)

add_executable(file_info_probe_protocol_tests
  tests/file_info_probe_protocol_tests.cpp
  src/ui/file_info_probe_client.cpp)
target_include_directories(file_info_probe_protocol_tests PRIVATE src/ui include)
target_compile_definitions(file_info_probe_protocol_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(file_info_probe_protocol_tests PRIVATE
  /W4 /permissive- /EHsc /utf-8 /UNDEBUG)

add_executable(builtin_file_info_tests
  tests/builtin_file_info_tests.cpp
  src/ui/file_info_probe_client.cpp)
target_include_directories(builtin_file_info_tests PRIVATE src/ui)
target_compile_definitions(builtin_file_info_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(builtin_file_info_tests PRIVATE
  /W4 /permissive- /EHsc /utf-8 /UNDEBUG)
target_link_libraries(builtin_file_info_tests PRIVATE ttplayer_core)
add_dependencies(builtin_file_info_tests ttplayer_file_info_probe)

add_executable(playlist_list_contract_tests
  tests/playlist_list_contract_tests.cpp)
target_compile_definitions(playlist_list_contract_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(playlist_list_contract_tests PRIVATE
  /W4 /permissive- /EHsc /utf-8 /UNDEBUG)
target_include_directories(playlist_list_contract_tests PRIVATE include)

add_executable(playlist_local_search_tests
  tests/playlist_local_search_tests.cpp)
target_compile_definitions(playlist_local_search_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(playlist_local_search_tests PRIVATE
  /W4 /permissive- /EHsc /utf-8 /UNDEBUG)
target_link_libraries(playlist_local_search_tests PRIVATE ttplayer_core)

# A deliberately deceptive legacy add-in: EnumInterface advertises the reader
# category and its raw object has a description slot, but QueryInterface for
# the reader IID fails. FUN_004CABC0 must not publish that description.
add_library(ttp_mock_incompatible SHARED tests/mock_incompatible_sound_addin.cpp)
target_compile_definitions(ttp_mock_incompatible PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(ttp_mock_incompatible PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(ttp_mock_incompatible PRIVATE ole32)
set_property(TARGET ttp_mock_incompatible PROPERTY MSVC_RUNTIME_LIBRARY MultiThreaded)

# A valid reader creator whose Open slot deliberately ignores cancellation for
# seven seconds.  It exercises AudioEngine's bounded UI wait without invoking
# undefined thread termination or depending on a network/device timeout.
add_library(ttp_mock_stalling SHARED tests/mock_stalling_sound_addin.cpp)
target_compile_definitions(ttp_mock_stalling PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(ttp_mock_stalling PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(ttp_mock_stalling PRIVATE ole32)
set_property(TARGET ttp_mock_stalling PROPERTY MSVC_RUNTIME_LIBRARY MultiThreaded)

add_executable(audio_recovery_tests tests/audio_recovery_tests.cpp)
add_executable(conversion_recovery_tests tests/conversion_recovery_tests.cpp
  src/app/exports.cpp)
target_compile_definitions(conversion_recovery_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(conversion_recovery_tests PRIVATE /W4 /permissive- /EHsc /utf-8 /UNDEBUG)
target_link_libraries(conversion_recovery_tests PRIVATE ttplayer_core)
ttplayer_stage_lame(conversion_recovery_tests)
target_compile_definitions(audio_recovery_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(audio_recovery_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(audio_recovery_tests PRIVATE ttplayer_core)
add_dependencies(audio_recovery_tests ttp_mock_incompatible ttp_mock_stalling)

foreach(revision_target IN ITEMS ttpcomm_mock_revision ttpcomm_mock_no_version)
  add_library(${revision_target} SHARED tests/mock_ttpcomm_revision.c)
  target_compile_options(${revision_target} PRIVATE /W4 /utf-8)
endforeach()
target_compile_definitions(ttpcomm_mock_no_version PRIVATE MOCK_NO_VERSION)

add_executable(ttpcomm_compatibility_tests
  tests/ttpcomm_compatibility_tests.cpp src/app/runtime.cpp)
target_compile_definitions(ttpcomm_compatibility_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(ttpcomm_compatibility_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(ttpcomm_compatibility_tests PRIVATE ttplayer_core ttpcomm_api)
add_dependencies(ttpcomm_compatibility_tests ttpcomm_mock_revision ttpcomm_mock_no_version)

add_executable(native_output_contract_tests tests/native_output_contract_tests.cpp)
target_include_directories(native_output_contract_tests PRIVATE src/ui)
target_compile_definitions(native_output_contract_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(native_output_contract_tests PRIVATE
  /W4 /permissive- /EHsc /utf-8)
target_link_libraries(native_output_contract_tests PRIVATE ttplayer_core)

add_executable(kernel_streaming_sink_tests tests/kernel_streaming_sink_tests.cpp)
target_compile_definitions(kernel_streaming_sink_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(kernel_streaming_sink_tests PRIVATE
  /W4 /permissive- /EHsc /utf-8)
target_link_libraries(kernel_streaming_sink_tests PRIVATE ttplayer_core)

add_executable(asio_sink_tests tests/asio_sink_tests.cpp)
target_compile_definitions(asio_sink_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(asio_sink_tests PRIVATE
  /W4 /permissive- /EHsc /utf-8 /UNDEBUG)
target_link_libraries(asio_sink_tests PRIVATE ttplayer_core)

add_executable(pcm_output_transform_tests
  tests/pcm_output_transform_tests.cpp)
target_compile_definitions(pcm_output_transform_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(pcm_output_transform_tests PRIVATE
  /W4 /permissive- /EHsc /utf-8 /UNDEBUG)
target_link_libraries(pcm_output_transform_tests PRIVATE ttplayer_core)

add_executable(equalizer_recovery_tests tests/equalizer_recovery_tests.cpp)
target_compile_definitions(equalizer_recovery_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
# This suite also uses assert() throughout; keep its Sandbox Release checks
# active for the same reason as ttplayer_tests above.
target_compile_options(equalizer_recovery_tests PRIVATE
  /W4 /permissive- /EHsc /utf-8 /UNDEBUG)
target_link_libraries(equalizer_recovery_tests PRIVATE ttplayer_core)

add_executable(discord_presence_tests tests/discord_presence_tests.cpp)
target_compile_definitions(discord_presence_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(discord_presence_tests PRIVATE
  /W4 /permissive- /EHsc /utf-8 /UNDEBUG)
target_link_libraries(discord_presence_tests PRIVATE ttplayer_core)

add_executable(discord_sync_tests tests/discord_sync_tests.cpp)
target_compile_definitions(discord_sync_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(discord_sync_tests PRIVATE /W4 /permissive- /EHsc /utf-8 /UNDEBUG)
target_link_libraries(discord_sync_tests PRIVATE ttplayer_core)

add_executable(ttpcomm_api_test ../reverse/tests/test_ttpcomm_api.c)
target_link_libraries(ttpcomm_api_test PRIVATE ttpcomm_api)

# Reverse-engineering probe for the legacy ttpGetSoundAddIn ABI. With optional
# sample paths it exercises creator/open/format/read-to-EOF/seek/read as well
# as reporting formats whose reader creators registered successfully.
add_executable(sound_addin_probe tools/probe_sound_addins.cpp)
target_compile_definitions(sound_addin_probe PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(sound_addin_probe PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(sound_addin_probe PRIVATE ttplayer_core)

# Destructive-by-design diagnostic for the exact ordinal-101 ReplayGain path.
# It is intentionally an explicit command-line probe and should be pointed at
# a disposable copy of an audio file because successful execution writes two
# metadata comments through the legacy add-in.
add_executable(replay_gain_probe tools/probe_replay_gain.cpp)
target_compile_definitions(replay_gain_probe PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(replay_gain_probe PRIVATE
  /W4 /permissive- /EHsc /utf-8)
target_link_libraries(replay_gain_probe PRIVATE ttplayer_core)

# End-to-end diagnostic for the decoder -> processor -> waveOut path.  Unlike
# the unit tests this uses explicit exit-status checks, so Release/NDEBUG
# builds exercise playback instead of compiling assert expressions away.
add_executable(audio_playback_probe tools/probe_audio_playback.cpp)
target_compile_definitions(audio_playback_probe PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(audio_playback_probe PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(audio_playback_probe PRIVATE ttplayer_core)

add_executable(progress_seek_tests tests/progress_seek_tests.cpp)
target_compile_definitions(progress_seek_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(progress_seek_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(progress_seek_tests PRIVATE ttplayer_core)

add_executable(taskbar_playback_tests tests/taskbar_playback_tests.cpp)
target_compile_definitions(taskbar_playback_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(taskbar_playback_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(taskbar_playback_tests PRIVATE ttplayer_core)

add_executable(skin_rebind_tests tests/ui/skin_rebind_tests.cpp)
target_compile_definitions(skin_rebind_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(skin_rebind_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(skin_rebind_tests PRIVATE ttplayer_core)

add_executable(playlist_marquee_tests tests/ui/playlist_marquee_tests.cpp
  src/app/resources.rc src/app/ttplayer.manifest)
target_compile_definitions(playlist_marquee_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(playlist_marquee_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(playlist_marquee_tests PRIVATE ttplayer_core)
add_test(NAME playlist_marquee_tests COMMAND playlist_marquee_tests "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_tests_properties(playlist_marquee_tests PROPERTIES TIMEOUT 30)

add_executable(pcm_integrity_probe tools/probe_pcm_integrity.cpp)
target_compile_definitions(pcm_integrity_probe PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(pcm_integrity_probe PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(pcm_integrity_probe PRIVATE ttplayer_core)

add_executable(ttpcomm_processor_probe tools/probe_ttpcomm_processors.cpp)
target_compile_definitions(ttpcomm_processor_probe PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(ttpcomm_processor_probe PRIVATE /W4 /permissive- /EHsc /utf-8)

# Deterministic cross-process OLE source for the isolated playlist ingress
# regression. It publishes a real CF_HDROP IDataObject and enters DoDragDrop;
# unlike WM_DROPFILES injection this proves that the destination registered an
# IDropTarget. Keep the CRT static so the helper itself also runs in a pristine
# Windows Sandbox image.
add_executable(ole_drop_source tools/windows_sandbox/ole_drop_source.cpp)
target_compile_definitions(ole_drop_source PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(ole_drop_source PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(ole_drop_source PRIVATE ole32 shell32 user32)
set_property(TARGET ole_drop_source PROPERTY MSVC_RUNTIME_LIBRARY MultiThreaded)
endif()

# The two legacy ttpres.dll files contain resources only: no code, entry point,
# imports, or exports.  Their recovered .res files are linked as /NOENTRY DLLs
# so resource type/name/language identities remain byte-for-byte compatible.
if(TTPLAYER_STAGE_RUNTIME)
set(TTPRES_RECOVERY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../reverse/ttpres")
set(TTPRES_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/resources")

add_custom_command(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/ttpcomm.dll"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/../ttpcomm.dll"
          "${CMAKE_CURRENT_BINARY_DIR}/ttpcomm.dll"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../ttpcomm.dll"
  VERBATIM
)

add_custom_command(
  OUTPUT "${TTPRES_OUTPUT_DIR}/root/ttpres.dll"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${TTPRES_OUTPUT_DIR}/root"
  COMMAND "${CMAKE_LINKER}" /NOLOGO /DLL /NOENTRY /MACHINE:X86 /NODEFAULTLIB
          /BASE:0x6FF50000
          "/OUT:${TTPRES_OUTPUT_DIR}/root/ttpres.dll"
          "${TTPRES_RECOVERY_DIR}/root/ttpres_original.res"
  DEPENDS "${TTPRES_RECOVERY_DIR}/root/ttpres_original.res"
  VERBATIM
)

add_custom_command(
  OUTPUT "${TTPRES_OUTPUT_DIR}/AddIn/ttpres.dll"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${TTPRES_OUTPUT_DIR}/AddIn"
  COMMAND "${CMAKE_LINKER}" /NOLOGO /DLL /NOENTRY /MACHINE:X86 /NODEFAULTLIB
          /BASE:0x6FF50000
          "/OUT:${TTPRES_OUTPUT_DIR}/AddIn/ttpres.dll"
          "${TTPRES_RECOVERY_DIR}/addin/ttpres_addin_original.res"
  DEPENDS "${TTPRES_RECOVERY_DIR}/addin/ttpres_addin_original.res"
  VERBATIM
)

add_custom_target(ttpres_resources ALL
  DEPENDS
    "${CMAKE_CURRENT_BINARY_DIR}/ttpcomm.dll"
    "${TTPRES_OUTPUT_DIR}/root/ttpres.dll"
    "${TTPRES_OUTPUT_DIR}/AddIn/ttpres.dll"
)

# The recovered program resolves runtime files strictly beside the executable.
# Keep the intermediate resource products for tests/inspection, then stage the
# exact runtime layout into every single- or multi-config output directory.
add_dependencies(ttplayer_rebuild ttpres_resources video_protocol_hls
                 video_protocol_web video_protocol_dash video_protocol_m3u
                 video_protocol_media)
add_custom_command(TARGET ttplayer_rebuild POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/../ttpcomm.dll"
          "$<TARGET_FILE_DIR:ttplayer_rebuild>/ttpcomm.dll"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${TTPRES_OUTPUT_DIR}/root/ttpres.dll"
          "$<TARGET_FILE_DIR:ttplayer_rebuild>/ttpres.dll"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory
          "${CMAKE_CURRENT_SOURCE_DIR}/../Skin"
          "$<TARGET_FILE_DIR:ttplayer_rebuild>/Skin"
  COMMAND "${CMAKE_COMMAND}" -E make_directory
          "$<TARGET_FILE_DIR:ttplayer_rebuild>/Skin/new"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory
          "${CMAKE_CURRENT_SOURCE_DIR}/../Icons"
          "$<TARGET_FILE_DIR:ttplayer_rebuild>/Icons"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory
          "${CMAKE_CURRENT_SOURCE_DIR}/../AddIn"
          "$<TARGET_FILE_DIR:ttplayer_rebuild>/AddIn"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory
          "${CMAKE_CURRENT_SOURCE_DIR}/../PlayList"
          "$<TARGET_FILE_DIR:ttplayer_rebuild>/PlayList"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${TTPRES_OUTPUT_DIR}/AddIn/ttpres.dll"
          "$<TARGET_FILE_DIR:ttplayer_rebuild>/AddIn/ttpres.dll"
  COMMAND "${CMAKE_COMMAND}"
          "-DSOURCE_DIRECTORY=${CMAKE_CURRENT_SOURCE_DIR}/.."
          "-DDESTINATION=$<TARGET_FILE_DIR:ttplayer_rebuild>"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/stage_settings.cmake"
  COMMAND "${CMAKE_COMMAND}"
          "-DPRESET_FILE=$<TARGET_FILE_DIR:ttplayer_rebuild>/AddIn/ttp_clienc.xml"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/repair_cli_presets.cmake"
  COMMAND "${CMAKE_COMMAND}" -E make_directory
          "$<TARGET_FILE_DIR:ttplayer_rebuild>/musicfree/libs"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory
          "${CMAKE_CURRENT_SOURCE_DIR}/src/musicfree/libs"
          "$<TARGET_FILE_DIR:ttplayer_rebuild>/musicfree/libs"
  VERBATIM
)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../mp3PRO.dll")
  add_custom_command(TARGET ttplayer_rebuild POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../mp3PRO.dll"
            "$<TARGET_FILE_DIR:ttplayer_rebuild>/mp3PRO.dll"
    VERBATIM)
endif()
endif()

if(BUILD_TESTING)
add_test(NAME build_date_tests
  COMMAND "${TTPLAYER_POWERSHELL}" -NoProfile -NonInteractive -ExecutionPolicy Bypass
          -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_build_date.ps1")
add_library(mp3pro_input_mock SHARED tests/mock_winamp_input.cpp tests/mock_winamp_input.def)
target_compile_definitions(mp3pro_input_mock PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(mp3pro_input_mock PRIVATE /W4 /EHsc /utf-8)
add_executable(mp3pro_source_tests tests/mp3pro_source_tests.cpp)
target_compile_definitions(mp3pro_source_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(mp3pro_source_tests PRIVATE /W4 /EHsc /utf-8 /UNDEBUG)
target_link_libraries(mp3pro_source_tests PRIVATE ttplayer_core)
add_dependencies(mp3pro_source_tests mp3pro_input_mock)
add_test(NAME mp3pro_source_tests COMMAND mp3pro_source_tests
    "$<TARGET_FILE:mp3pro_input_mock>" "${CMAKE_CURRENT_SOURCE_DIR}/../mp3PRO.dll")
set_tests_properties(mp3pro_source_tests PROPERTIES TIMEOUT 60)
add_test(NAME ttplayer_tests COMMAND ttplayer_tests)
add_test(NAME shell_send_to_tests COMMAND shell_send_to_tests)
add_test(NAME playlist_network_commands_tests
         COMMAND playlist_network_commands_tests)
add_test(NAME file_info_probe_protocol_tests
         COMMAND file_info_probe_protocol_tests)
add_test(NAME builtin_file_info_tests COMMAND builtin_file_info_tests)
add_test(NAME file_association_tests COMMAND file_association_tests)
add_test(NAME playlist_list_contract_tests
         COMMAND playlist_list_contract_tests)
add_test(NAME playlist_local_search_tests
         COMMAND playlist_local_search_tests)
add_test(NAME desktop_lyrics_tests COMMAND desktop_lyrics_tests
         "${CMAKE_CURRENT_SOURCE_DIR}/..")
add_test(NAME audio_recovery_tests COMMAND audio_recovery_tests
         "${CMAKE_CURRENT_SOURCE_DIR}/..")
add_test(NAME conversion_recovery_tests COMMAND conversion_recovery_tests
         "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_tests_properties(conversion_recovery_tests PROPERTIES TIMEOUT 60)
add_test(NAME progress_seek_tests COMMAND progress_seek_tests
         "${CMAKE_CURRENT_SOURCE_DIR}/..")
add_test(NAME taskbar_playback_tests COMMAND taskbar_playback_tests)
add_test(NAME skin_rebind_tests COMMAND skin_rebind_tests "${CMAKE_CURRENT_SOURCE_DIR}/..")
add_test(NAME native_output_contract_tests COMMAND native_output_contract_tests)
add_test(NAME kernel_streaming_sink_tests COMMAND kernel_streaming_sink_tests)
add_test(NAME asio_sink_tests COMMAND asio_sink_tests)
add_test(NAME pcm_output_transform_tests COMMAND pcm_output_transform_tests
         "${CMAKE_CURRENT_SOURCE_DIR}/..")
add_test(NAME equalizer_recovery_tests COMMAND equalizer_recovery_tests
         "${CMAKE_CURRENT_SOURCE_DIR}/..")
add_test(NAME discord_presence_tests COMMAND discord_presence_tests)
add_test(NAME discord_sync_tests COMMAND discord_sync_tests)
set_tests_properties(discord_presence_tests discord_sync_tests PROPERTIES TIMEOUT 45)
add_test(NAME ttpcomm_api_test COMMAND ttpcomm_api_test)
add_test(NAME ttpcomm_compatibility_tests COMMAND ttpcomm_compatibility_tests
         "$<TARGET_FILE:ttpcomm_mock_revision>"
         "$<TARGET_FILE:ttpcomm_mock_no_version>")
set_tests_properties(ttpcomm_compatibility_tests PROPERTIES
  WORKING_DIRECTORY "$<TARGET_FILE_DIR:ttplayer_rebuild>")
add_test(NAME ttplayer_ui_smoke COMMAND ttplayer_rebuild --smoke-test)
add_test(NAME ttpcomm_processor_probe COMMAND ttpcomm_processor_probe
         "$<TARGET_FILE_DIR:ttplayer_rebuild>/ttpcomm.dll")
add_test(NAME dsp_probe_tests COMMAND dsp_probe_tests
         "$<TARGET_FILE:ttplayer_dsp_probe>"
         "$<TARGET_FILE:dsp_probe_valid>"
         "$<TARGET_FILE:dsp_probe_old>"
         "$<TARGET_FILE:dsp_probe_stall>")
add_test(NAME embedded_dsp_probe_tests COMMAND dsp_probe_tests
         "$<TARGET_FILE:ttplayer_rebuild>"
         "$<TARGET_FILE:dsp_probe_valid>"
         "$<TARGET_FILE:dsp_probe_old>"
         "$<TARGET_FILE:dsp_probe_stall>" --embedded)
add_test(NAME portable_worker_tests COMMAND portable_worker_tests
         "$<TARGET_FILE:ttplayer_rebuild>"
         "$<TARGET_FILE:dsp_probe_configure>"
         "$<TARGET_FILE:ttplayer_output_device_probe>")
set_tests_properties(portable_worker_tests embedded_dsp_probe_tests PROPERTIES TIMEOUT 45)
add_test(NAME winamp_dsp_runtime_tests COMMAND winamp_dsp_runtime_tests
         "$<TARGET_FILE:dsp_probe_valid>"
         "$<TARGET_FILE:dsp_probe_old>"
         "$<TARGET_FILE:dsp_probe_fault>"
         "$<TARGET_FILE:dsp_probe_pacemaker>")
set_tests_properties(ttplayer_ui_smoke PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)
endif()

# MusicFree 兼容层的纯 JS 依赖库（crypto-js/dayjs/qs/big-integer/he）必须与该 EXE
# 同级部署：插件在 QuickJS 沙箱里 require() 这些包。与 TTPLAYER_STAGE_RUNTIME
# 开关无关，始终执行，避免关闭 stage 时「音乐窗」搜索因缺库直接失败。
if(TARGET ttplayer_rebuild)
  add_custom_command(TARGET ttplayer_rebuild POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_FILE_DIR:ttplayer_rebuild>/musicfree/libs"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/src/musicfree/libs"
            "$<TARGET_FILE_DIR:ttplayer_rebuild>/musicfree/libs"
    VERBATIM
  )
endif()

# WebView2Loader.dll：现代版链接导入库、启动时静态导入；XP/Win7 版不链接导入库
# 但用 LoadLibrary 动态解析 CreateCoreWebView2EnvironmentWithOptions，同样需要
# 这个 DLL 在 exe 同目录。只复制 build/native/x86 下的 dll（32 位约束），不下载。
if(TARGET ttplayer_rebuild)
  add_custom_command(TARGET ttplayer_rebuild POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/webview2/build/native/x86/WebView2Loader.dll"
            "$<TARGET_FILE_DIR:ttplayer_rebuild>/WebView2Loader.dll"
    COMMENT "Deploying WebView2Loader.dll beside the exe"
    VERBATIM
  )
endif()
# 部署目录 TTPlayer/ 只给现代版写，避免兼容版构建覆盖正在运行的现代版目录。
if(TARGET ttplayer_rebuild AND NOT TTPLAYER_LEGACY_WINDOWS)
  add_custom_command(TARGET ttplayer_rebuild POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/webview2/build/native/x86/WebView2Loader.dll"
            "${CMAKE_CURRENT_SOURCE_DIR}/../../TTPlayer/WebView2Loader.dll"
    COMMENT "Deploying WebView2Loader.dll into TTPlayer/"
    VERBATIM
  )
endif()

# ── VC++ 运行时 app-local 部署 ──────────────────────────────────────────────
# 为什么必须「自带」而不是「启动后检测再安装」：主 exe 与 AddIn 插件都是 /MD
# 动态链接，缺 MSVCP140 时是 **Windows 加载器** 直接拒绝启动（弹系统错误框，
# 进程根本没起来），我们自己的检测代码一行都执行不到。所以唯一可靠的办法是
# 按微软官方支持的方式做 app-local 部署：把运行时 DLL 放到 exe 同目录，加载器
# 优先在应用目录找到它们，目标机器无需安装任何 VC++ 运行时。
#
# 依赖清单由 dumpbin /DEPENDENTS 实测得出（exe + 4 个协议插件 + webp 插件）：
#   MSVCP140 / MSVCP140_ATOMIC_WAIT / VCRUNTIME140 为直接依赖，
#   VCRUNTIME140_1 为 MSVCP140 的间接依赖（异常处理），一并带上。
# 运行时目录从编译器路径反推（.../VC/Tools/MSVC/<ver>/bin/... → .../VC/Redist/MSVC），
# 不硬编码 VS 版本号；找不到时只警告不中断构建（老手可自行拷 DLL）。
if(TARGET ttplayer_rebuild AND NOT TTPLAYER_LEGACY_WINDOWS)
  string(REGEX REPLACE "/VC/Tools/MSVC/.*$" "/VC" _vc_root
         "${CMAKE_CXX_COMPILER}")
  string(REPLACE "\\" "/" _vc_root "${_vc_root}")
  file(GLOB _crt_dirs "${_vc_root}/Redist/MSVC/*/x86/Microsoft.VC*.CRT")
  if(_crt_dirs)
    list(SORT _crt_dirs)
    list(GET _crt_dirs -1 _crt_dir)      # 取最高版本
    set(_crt_files
      msvcp140.dll
      vcruntime140.dll
      vcruntime140_1.dll
      msvcp140_atomic_wait.dll)
    set(_crt_copied "")
    foreach(_f ${_crt_files})
      if(EXISTS "${_crt_dir}/${_f}")
        list(APPEND _crt_copied "${_f}")
        add_custom_command(TARGET ttplayer_rebuild POST_BUILD
          COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                  "${_crt_dir}/${_f}"
                  "$<TARGET_FILE_DIR:ttplayer_rebuild>/${_f}"
          COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                  "${_crt_dir}/${_f}"
                  "${CMAKE_CURRENT_SOURCE_DIR}/../../TTPlayer/${_f}"
          COMMENT "Deploying VC runtime (app-local): ${_f}"
          VERBATIM
        )
      endif()
    endforeach()
    message(STATUS "VC 运行时 app-local 部署: ${_crt_dir} -> ${_crt_copied}")
  else()
    message(WARNING
      "未找到 VC 运行时 redist 目录（${_vc_root}/Redist/MSVC/*/x86/Microsoft.VC*.CRT），"
      "已跳过 app-local 部署；目标机器需自行安装 VC++ 2015-2022 运行库。")
  endif()
endif()
