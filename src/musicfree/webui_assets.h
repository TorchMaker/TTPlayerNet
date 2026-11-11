#pragma once

// 音乐窗 WebView2 页面资产 ——「外部覆盖优先 + 嵌资源兜底」双源加载（Task 2.3）
//
// 【职责】判定并准备 WebView2 可直接映射的页面根目录：
//   1) 外部覆盖优先：<exe>\webui\index.html 存在 → 直接用该目录（改页面免重编 exe）；
//   2) 否则把嵌在 exe 内的 RT_RCDATA 三件套（IDR_WEBUI_INDEX/STYLE/SCRIPT）
//      解包到 %LOCALAPPDATA%\TTPlayer\webui\；解包后写 version.marker
//      （内容为三个资源的尺寸组合，如 3001=1234;3002=567;3003=890），
//      下次启动比对 marker 一致且三个文件都在则跳过重写，变了就重新解包；
//   3) 解包目录无权限/写失败 → 回退 %TEMP%\TTPlayer\webui；
//      两条都失败返回空串（宿主拿到空串后自行回退）。
//      全路径绝不抛异常、绝不崩溃。
//
// 【资源 ID】必须与 src/app/resources.rc 中的 RT_RCDATA 条目一致（勿单侧修改）。
//
// 【日志】与 src/video/protocols/protocol_web.cpp 的 AppendProtoFailLog 同款
//   路径算法（GetModuleFileNameW(nullptr) 定位 exe 目录）落一行到
//   <exe>\video\bridge.log，记录模式与目录；只在失败与模式切换时写（内容去重）。
//
// 调用方（musicfree_webview_host）拿到返回的绝对目录后交
// SetVirtualHostNameToFolderMapping 使用。

#include <string>

// 资源 ID：与 src/app/resources.rc 中的定义保持一致。
#define IDR_WEBUI_INDEX 3001
#define IDR_WEBUI_STYLE 3002
#define IDR_WEBUI_SCRIPT 3003

namespace ttplayer::musicfree {

// 判定并准备页面根目录：外部 <exe>\webui\ 存在→直接用；
// 否则把嵌资源 IDR_WEBUI_INDEX/STYLE/SCRIPT 解包到 %LOCALAPPDATA%\TTPlayer\webui\
// （写一个 version.marker，内容为资源尺寸组合，变了就重新解包）。
// 返回可供 SetVirtualHostNameToFolderMapping 使用的**绝对目录**；失败返回空串。
std::wstring PrepareWebuiRoot();

// 返回实际使用模式描述（"external" / "embedded" / ""），供日志
const char* WebuiMode();

}  // namespace ttplayer::musicfree
