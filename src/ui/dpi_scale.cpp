#include "ttplayer/ui/dpi_scale.h"

#include "ttplayer/platform/optional_windows_api.h"

#include <cstdlib>
#include <string>

namespace ttplayer::ui {
namespace {

int OverrideDpi() {
    // _dupenv_s keeps the /W4 build clean (getenv is C4996 under MSVC).
    char* buffer = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&buffer, &size, "TTPLAYER_DPI_OVERRIDE") != 0 || !buffer)
        return 0;
    const int value = std::atoi(buffer);
    std::free(buffer);
    if (value < 64 || value > 480) return 0;
    return value;
}

} // namespace

DpiScale ResolveScaleForWindow(HWND hwnd) {
    if (const int override_dpi = OverrideDpi()) return DpiScale{override_dpi};

    if (hwnd) {
        // Both helpers are plain functions that resolve their import lazily
        // and fall back to 0 / a failing HRESULT when unavailable.
        const UINT dpi = ttplayer::platform::GetDpiForWindow(hwnd);
        if (dpi) return DpiScale{static_cast<int>(dpi)};

        UINT effective_x = 0;
        UINT effective_y = 0;
        // MDT_EFFECTIVE_DPI = 0
        if (SUCCEEDED(ttplayer::platform::GetDpiForMonitor(
                MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), 0,
                &effective_x, &effective_y)) &&
            effective_x) {
            return DpiScale{static_cast<int>(effective_x)};
        }
    }
    const HDC dc = GetDC(nullptr);
    const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    return DpiScale{dpi};
}

NONCLIENTMETRICSW NonClientMetricsForDpi(int dpi) {
    NONCLIENTMETRICSW metrics{};
    // Query the pre-padded-border layout first: that is the shape the original
    // code reads and Vista+ still accepts it.
    metrics.cbSize = sizeof(metrics) - sizeof(metrics.iPaddedBorderWidth);
    const bool dpi_aware =
        dpi > 0 &&
        ttplayer::platform::SystemParametersInfoForDpi(
            SPI_GETNONCLIENTMETRICS, metrics.cbSize, &metrics, 0,
            static_cast<UINT>(dpi)) != FALSE;
    if (!dpi_aware &&
        !SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, metrics.cbSize,
                               &metrics, 0)) {
        metrics.cbSize = sizeof(metrics);
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, metrics.cbSize,
                              &metrics, 0);
    }
    return metrics;
}

int SystemMetricForDpi(int index, int dpi) {
    if (dpi > 0) {
        if (const int value = ttplayer::platform::GetSystemMetricsForDpi(
                index, static_cast<UINT>(dpi)))
            return value;
    }
    return ::GetSystemMetrics(index);
}

namespace {

// Seeded from the system DPI: skin packages decode their bitmaps before any
// window exists. PlayerWindow::ApplyDpiScale refreshes it afterwards.
int g_session_dpi = 0;

} // namespace

int SessionDpi() {
    if (g_session_dpi <= 0)
        g_session_dpi = ResolveScaleForWindow(nullptr).dpi;
    return g_session_dpi > 0 ? g_session_dpi : 96;
}

void SetSessionDpi(int dpi) {
    if (dpi >= 64 && dpi <= 480) g_session_dpi = dpi;
}

} // namespace ttplayer::ui
