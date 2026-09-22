// Headless High-DPI geometry regression (`--dpi-selftest`).
//
// The visual acceptance pass for High-DPI needs a real 150%/200% monitor, which
// no CI or scripted run can provide. Most of what "looks wrong" on such a
// screen, however, is a *geometry* invariant that can be checked without a
// display at all. This module turns those into assertions:
//
//   1. Anti-accumulation  — AtDpi(96) is the identity and moving the live
//      mirror to another DPI and back reproduces the original geometry. This
//      is the most valuable check: it fails loudly the moment anything derives
//      a mirror from the already-scaled `skin_` instead of the logical source
//      (the "double scaling" trap, see docs/HIGH_DPI_SKIN_SPEC.md §11.3).
//   2. Linearity          — window size tracks dpi/96 within 1px.
//   3. Monotonicity       — nothing shrinks as the DPI grows.
//   4. Rectangle sanity   — no inverted bounds anywhere in the scaled view.
//   5. Font scaling       — LOGFONT.lfHeight keeps its sign and grows.
//   6. Region validity    — CreateWindowRegion still yields a real region.
//   7. Config migration   — saved rectangles survive a 96 -> 192 -> 96 trip.
//
// Results go to stdout (always) and to the trace file (when TTPLAYER_MF_TRACE
// is set), and the process exit code is 1 when anything fails.

#include "ttplayer/ui/player_window.h"

#include <cstdio>
#include <string>
#include <vector>

#include "ttplayer/musicfree/musicfree_app.h"
#include "ttplayer/ui/dpi_scale.h"

namespace ttplayer::ui {
namespace {

constexpr int kTestDpis[] = {96, 120, 144, 168, 192, 240};

std::string SizeText(SIZE value) {
    return std::to_string(value.cx) + "x" + std::to_string(value.cy);
}

std::string RectText(const RECT& value) {
    return std::to_string(value.left) + "," + std::to_string(value.top) + "," +
           std::to_string(value.right) + "," + std::to_string(value.bottom);
}

bool SameSize(SIZE a, SIZE b) { return a.cx == b.cx && a.cy == b.cy; }

bool SameRect(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right &&
           a.bottom == b.bottom;
}

bool Inverted(const RECT& value) {
    return value.right < value.left || value.bottom < value.top;
}

} // namespace

bool PlayerWindow::RunDpiSelfTest() {
    std::vector<std::string> failures;
    std::string report;
    const auto note = [&report](const std::string& line) {
        report += line;
        report += '\n';
    };
    const auto fail = [&failures, &note](const std::string& line) {
        failures.push_back(line);
        note("FAIL  " + line);
    };

    if (!logical_skin_) {
        note("logical_skin=0 (no skin loaded; nothing to check)");
        std::printf("%s\n", report.c_str());
        musicfree::TraceDiagnostic("DPI_SELFTEST", report);
        return false;
    }

    const SIZE source_window = logical_skin_->WindowSize();
    const SIZE source_mini = logical_skin_->MiniWindowSize();
    note("source_window=" + SizeText(source_window) +
         " source_mini=" + SizeText(source_mini));

    // ---- 1. Anti-accumulation -------------------------------------------
    // AtDpi(96) must be the identity, and migrating the live mirror to another
    // DPI and back must land on exactly the values we started from. The second
    // half is the real regression test for the "double scaling" trap: it fails
    // the moment anything derives the new mirror from `skin_` (already scaled)
    // instead of from `logical_skin_`.
    // Note AtDpi() is a *scale by factor* operation, so chaining
    // AtDpi(120).AtDpi(96) is expected to keep the 120 values — that is not the
    // property under test here.
    {
        const auto identity = logical_skin_->AtDpi(ui::DpiScale{96});
        if (!SameSize(identity.WindowSize(), source_window))
            fail("AtDpi(96) is not the identity: got " +
                 SizeText(identity.WindowSize()) + " want " +
                 SizeText(source_window));
        if (!SameSize(identity.MiniWindowSize(), source_mini))
            fail("AtDpi(96) changed the mini window: got " +
                 SizeText(identity.MiniWindowSize()) + " want " +
                 SizeText(source_mini));

        const int start = current_scale_.dpi > 0 ? current_scale_.dpi : 96;
        const int other = start >= 192 ? 96 : 192;
        const auto expected = logical_skin_->AtDpi(ui::DpiScale{start});
        ApplyDpiScale(ui::DpiScale{other});
        ApplyDpiScale(ui::DpiScale{start});
        if (!skin_) {
            fail("mirror lost after the DPI round trip " +
                 std::to_string(start) + "->" + std::to_string(other) + "->" +
                 std::to_string(start));
        } else {
            if (!SameSize(skin_->WindowSize(), expected.WindowSize()))
                fail("round trip accumulated: got " +
                     SizeText(skin_->WindowSize()) + " want " +
                     SizeText(expected.WindowSize()));
            if (!SameSize(skin_->MiniWindowSize(), expected.MiniWindowSize()))
                fail("mini round trip accumulated: got " +
                     SizeText(skin_->MiniWindowSize()) + " want " +
                     SizeText(expected.MiniWindowSize()));
            if (skin_->Elements().size() != expected.Elements().size()) {
                fail("element count changed across the round trip");
            } else {
                for (size_t index = 0; index < expected.Elements().size();
                     ++index) {
                    const RECT got = skin_->Elements()[index].bounds;
                    const RECT want = expected.Elements()[index].bounds;
                    if (!SameRect(got, want))
                        fail("element[" + std::to_string(index) +
                             "] accumulated across the round trip: " +
                             RectText(got) + " want " + RectText(want));
                }
            }
        }
        note("round_trip=" + std::to_string(start) + "->" +
             std::to_string(other) + "->" + std::to_string(start));
    }

    // ---- 2/3/4/5/6. Per-DPI invariants ----------------------------------
    SIZE previous_window{};
    LONG previous_font = 0;
    bool have_previous = false;
    for (const int dpi : kTestDpis) {
        const ui::DpiScale scale{dpi};
        const auto mirror = logical_skin_->AtDpi(scale);
        const SIZE window = mirror.WindowSize();

        // 2. Linearity (1px tolerance: MulDiv rounds each axis independently).
        const SIZE expected{::MulDiv(source_window.cx, dpi, 96),
                            ::MulDiv(source_window.cy, dpi, 96)};
        if (std::abs(window.cx - expected.cx) > 1 ||
            std::abs(window.cy - expected.cy) > 1)
            fail("non-linear window size at dpi=" + std::to_string(dpi) +
                 ": got " + SizeText(window) + " want " + SizeText(expected) +
                 " (+/-1)");

        // 3. Monotonicity (same 1px slack for rounding).
        if (have_previous &&
            (window.cx < previous_window.cx - 1 ||
             window.cy < previous_window.cy - 1))
            fail("window shrank at dpi=" + std::to_string(dpi) + ": " +
                 SizeText(window) + " after " + SizeText(previous_window));
        previous_window = window;

        // 4. No inverted rectangles anywhere in the scaled view.
        size_t inverted = 0;
        for (const auto& element : mirror.Elements())
            if (Inverted(element.bounds)) ++inverted;
        if (Inverted(mirror.Lyric().lyric_bounds)) ++inverted;
        if (Inverted(mirror.Playlist().list_bounds)) ++inverted;
        if (Inverted(mirror.Playlist().toolbar_bounds)) ++inverted;
        if (inverted != 0)
            fail(std::to_string(inverted) + " inverted rect(s) at dpi=" +
                 std::to_string(dpi));

        // 5. Font height keeps its sign and never shrinks.
        const LONG font_height = mirror.Lyric().font.lfHeight;
        const LONG source_font = logical_skin_->Lyric().font.lfHeight;
        if (source_font != 0) {
            if ((font_height < 0) != (source_font < 0))
                fail("font sign flipped at dpi=" + std::to_string(dpi) + ": " +
                     std::to_string(font_height) + " vs source " +
                     std::to_string(source_font));
            if (have_previous && std::abs(font_height) < std::abs(previous_font))
                fail("font shrank at dpi=" + std::to_string(dpi) + ": " +
                     std::to_string(font_height));
        }
        previous_font = font_height;

        // 6. The shaped window region must still exist at every DPI.
        if (const HRGN region = mirror.CreateWindowRegion(false, scale)) {
            RECT box{};
            if (GetRgnBox(region, &box) <= NULLREGION)
                fail("empty window region at dpi=" + std::to_string(dpi));
            DeleteObject(region);
        } else {
            fail("CreateWindowRegion returned null at dpi=" +
                 std::to_string(dpi));
        }

        have_previous = true;
        note("dpi=" + std::to_string(dpi) + " window=" + SizeText(window) +
             " font=" + std::to_string(font_height) +
             " elements=" + std::to_string(mirror.Elements().size()));
    }

    // ---- 7. Saved-geometry migration round trip --------------------------
    {
        const RECT original = settings_.player.player_window;
        const int original_dpi = settings_.player.window_dpi;
        settings_.player.window_dpi = 96;
        settings_.player.player_window = RECT{100, 100, 500, 400};
        MigrateSavedWindowGeometry(192);
        const RECT grown = settings_.player.player_window;
        const RECT want_grown{200, 200, 1000, 800};
        if (!SameRect(grown, want_grown))
            fail("migration 96->192 wrong: " + RectText(grown) + " want " +
                 RectText(want_grown));
        MigrateSavedWindowGeometry(96);
        const RECT back = settings_.player.player_window;
        const RECT want_back{100, 100, 500, 400};
        if (!SameRect(back, want_back))
            fail("migration round trip not symmetric: " + RectText(back) +
                 " want " + RectText(want_back));
        settings_.player.player_window = original;
        settings_.player.window_dpi = original_dpi;
        note("migration round_trip=" +
             std::string(SameRect(back, want_back) ? "ok" : "broken"));
    }

    // ---- 8. Per-window mirrors (strategy C) -----------------------------
    // The auxiliary windows resolve their own monitor DPI through SkinForDpi.
    // Verify the cache returns a distinct, correctly scaled mirror per DPI and
    // that repeat lookups are stable (a cache that rebuilt every time would
    // silently re-derive geometry mid-paint).
    {
        const auto* at144 = SkinForDpi(144);
        const auto* at192 = SkinForDpi(192);
        if (!at144 || !at192) {
            fail("SkinForDpi returned null (144=" +
                 std::to_string(at144 != nullptr) + " 192=" +
                 std::to_string(at192 != nullptr) + ")");
        } else {
            if (at144 == at192)
                fail("SkinForDpi collapsed 144 and 192 onto one mirror");
            const auto want144 = logical_skin_->AtDpi(ui::DpiScale{144});
            const auto want192 = logical_skin_->AtDpi(ui::DpiScale{192});
            if (!SameSize(at144->WindowSize(), want144.WindowSize()))
                fail("SkinForDpi(144) mismatch: " +
                     SizeText(at144->WindowSize()) + " want " +
                     SizeText(want144.WindowSize()));
            if (!SameSize(at192->WindowSize(), want192.WindowSize()))
                fail("SkinForDpi(192) mismatch: " +
                     SizeText(at192->WindowSize()) + " want " +
                     SizeText(want192.WindowSize()));
            if (SkinForDpi(144) != at144)
                fail("SkinForDpi is not cached (second lookup moved)");
            note("skin_for_dpi 144=" + SizeText(at144->WindowSize()) +
                 " 192=" + SizeText(at192->WindowSize()));
        }
    }

    const bool ok = failures.empty();
    note(failures.empty() ? "result=ok" : "result=" +
         std::to_string(failures.size()) + " failure(s)");
    std::printf("DPI_SELFTEST %s\n%s", ok ? "ok=1" : "ok=0", report.c_str());
    musicfree::TraceDiagnostic("DPI_SELFTEST", report);
    return ok;
}

} // namespace ttplayer::ui
