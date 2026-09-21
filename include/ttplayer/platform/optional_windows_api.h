#pragma once

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <propsys.h>
#include <shobjidl.h>
#include <commctrl.h>

struct IWMSyncReader;

// These components are optional on XP (and Windows N without the media pack).
// Keep their functions out of the process import table. Unsupported operations
// return E_NOTIMPL so existing decoder/metadata fallbacks can handle them.
namespace ttplayer::platform {
bool HasMediaFoundation();
HRESULT WMCreateSyncReader(IUnknown* certificate, DWORD rights, IWMSyncReader** reader);
HRESULT SHGetPropertyStoreFromParsingName(PCWSTR path, IBindCtx* context,
    GETPROPERTYSTOREFLAGS flags, REFIID iid, void** store);
HRESULT TaskDialogIndirect(const TASKDIALOGCONFIG* config, int* button, int* radio, BOOL* checked);
IStream* SHCreateMemStream(const BYTE* bytes, UINT size);
HRESULT MFStartup(ULONG version, DWORD flags);
HRESULT MFShutdown();
HRESULT MFCreateAttributes(IMFAttributes** attributes, UINT32 size);
HRESULT MFCreateMediaType(IMFMediaType** type);
HRESULT MFCreateMFByteStreamOnStream(IStream* stream, IMFByteStream** result);
HRESULT MFCreateSourceReaderFromByteStream(IMFByteStream* stream,
    IMFAttributes* attributes, IMFSourceReader** reader);
HRESULT MFCreateSourceReaderFromURL(LPCWSTR url, IMFAttributes* attributes,
    IMFSourceReader** reader);
HRESULT PropVariantToUInt32(REFPROPVARIANT value, ULONG* result);
HRESULT PropVariantToUInt64(REFPROPVARIANT value, ULONGLONG* result);
HRESULT PropVariantToString(REFPROPVARIANT value, PWSTR result, UINT size);
HRESULT PropVariantToStringAlloc(REFPROPVARIANT value, PWSTR* result);

// High-DPI awareness. These are absent on XP/Win7; resolving them dynamically
// keeps the import table clean and lets the player fall back to system-DPI
// awareness. See docs/HIGH_DPI_SKIN_SPEC.md §9.
BOOL SetProcessDpiAwarenessContext(void* value);
HRESULT SetProcessDpiAwareness(int value);
UINT GetDpiForWindow(HWND hwnd);
HRESULT GetDpiForMonitor(HMONITOR monitor, int dpi_type, UINT* dpi_x, UINT* dpi_y);
UINT GetDpiForSystem();
int GetSystemMetricsForDpi(int index, UINT dpi);
BOOL SystemParametersInfoForDpi(UINT action, UINT param, void* pvparam,
                                UINT fwinini, UINT dpi);
} // namespace ttplayer::platform
