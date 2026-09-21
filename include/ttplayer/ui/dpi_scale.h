#pragma once

#include <windows.h>

namespace ttplayer::ui {

// Single source of truth for converting 96-DPI "logical" skin coordinates into
// physical pixels at the current monitor's DPI. Every conversion goes through
// here so a skin object can stay in logical coordinates and only the
// consumption boundary produces physical pixels (see docs/HIGH_DPI_SKIN_SPEC.md).
struct DpiScale {
    int dpi = 96;
    static constexpr int kBaseDpi = 96;

    [[nodiscard]] double factor() const noexcept {
        return static_cast<double>(dpi) / kBaseDpi;
    }
    [[nodiscard]] int X(int logical) const noexcept {
        return ::MulDiv(logical, dpi, kBaseDpi);
    }
    [[nodiscard]] int Y(int logical) const noexcept {
        return ::MulDiv(logical, dpi, kBaseDpi);
    }
    [[nodiscard]] SIZE Size(SIZE s) const noexcept { return {X(s.cx), Y(s.cy)}; }
    [[nodiscard]] RECT Rect(RECT r) const noexcept {
        return {X(r.left), Y(r.top), X(r.right), Y(r.bottom)};
    }
    // XML font_size is a pixel height assigned directly to LOGFONT.lfHeight,
    // not a point size.
    [[nodiscard]] int FontPx(int px) const noexcept {
        return ::MulDiv(px, dpi, kBaseDpi);
    }
    // LOGFONT.lfHeight may be negative (an em-height). Preserve the sign so
    // font metrics stay correct under scaling.
    [[nodiscard]] int LfHeight(int lf) const noexcept {
        return lf < 0 ? -::MulDiv(-lf, dpi, kBaseDpi) : ::MulDiv(lf, dpi, kBaseDpi);
    }
};

// Resolve the effective DPI for a window. Honors TTPLAYER_DPI_OVERRIDE
// (an explicit DPI value, e.g. 144 for 150%) so the scaling logic can be
// exercised on a 100% development machine. Falls back through
// GetDpiForWindow -> GetDpiForMonitor -> system LOGPIXELSX (XP/Win7).
DpiScale ResolveScaleForWindow(HWND hwnd);

// Per-monitor variant of SPI_GETNONCLIENTMETRICS. Under Per-Monitor V2 the
// plain system query answers with the *system* DPI, so an owner-drawn menu
// built from it uses the wrong font size on a secondary monitor. Falls back to
// the system query when the DPI-aware entry point is unavailable (Win8.1/XP).
// Owner-drawn menus must rebuild their font after every WM_DPICHANGED.
NONCLIENTMETRICSW NonClientMetricsForDpi(int dpi);

// Per-monitor GetSystemMetrics. Returns the system-DPI value when the
// DPI-aware entry point is unavailable (Win8.1/XP).
int SystemMetricForDpi(int index, int dpi);

// The DPI that newly decoded assets should target. Skin packages pick their
// @Nx variant when they load, long before any HWND exists, so the value is
// seeded from the system DPI and refreshed whenever the main window changes
// monitor. Already-decoded bitmaps are NOT re-picked (see the spec §7).
int SessionDpi();
void SetSessionDpi(int dpi);

} // namespace ttplayer::ui
