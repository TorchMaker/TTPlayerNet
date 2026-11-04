#pragma once
#include <windows.h>
#include <filesystem>
namespace ttplayer::ui {
inline constexpr UINT kMsgArchivePassword=WM_APP+0x37a;
inline constexpr UINT kMsgArchivePasswordStorageFailure=WM_APP+0x37b;
void InstallArchivePasswordDialog(HWND,const std::filesystem::path&);
void DispatchArchivePasswordDialog();
// UI thread only. Keep requests routed to the main window while a progress
// dialog owns the nested password interaction.
void SetArchivePasswordModalParent(HWND);
void ReportArchivePasswordStorageFailure(HRESULT);
void CloseArchivePasswordDialog();
}
