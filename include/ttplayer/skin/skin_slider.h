#pragma once

#include <algorithm>
#include <windows.h>

namespace ttplayer::skin {

// 00428F41 and 00452045 deliberately use different vertical intervals.
// The hit strip spans the whole cross-axis, even when the bitmap is smaller.
struct SkinSliderGeometry {
    RECT bounds{};
    int minimum{};
    int maximum{100};
    int extent{1}; // null bitmap = 1; a valid zero-width frame stays zero
    bool vertical{};
    int step{1};

    RECT Thumb(int value) const {
        value = std::clamp(value, minimum, maximum);
        RECT strip = bounds;
        const int range = maximum == minimum ? 1 : maximum - minimum;
        const int size = vertical ? bounds.bottom - bounds.top : bounds.right - bounds.left;
        const int offset = 1 + MulDiv(vertical ? maximum - value : value - minimum,
                                      size - 2 - extent, range);
        const int first = std::max(0, offset);
        const int last = offset < 0 ? 0 : offset + extent;
        if (vertical) { strip.top += first; strip.bottom = bounds.top + last; }
        else { strip.left += first; strip.right = bounds.left + last; }
        return strip;
    }

    int ValueAt(POINT point) const {
        const int first = (vertical ? bounds.top : bounds.left + 1) + extent / 2;
        const int last = (vertical ? bounds.bottom : bounds.right - 1) + extent / 2 - extent;
        const int interval = std::max(1, last - first);
        const int mapped = MulDiv((vertical ? point.y : point.x) - first,
                                   maximum - minimum, interval);
        return std::clamp(vertical ? maximum - mapped : minimum + mapped,
                          minimum, maximum);
    }
};

struct SkinSliderEvent {
    int notification{-1}; // SB_THUMBPOSITION, SB_THUMBTRACK, SB_ENDSCROLL
    bool redraw{};
};

// Each logical/native slider owns this state; the window adapter owns capture,
// focus and the host-specific meaning of the scroll notification.
struct SkinSliderState {
    int value{};
    int initial{};
    POINT grab{};
    bool dragging{};
    bool captured{};
    bool commit_pending{};

    void Sync(const SkinSliderGeometry& geometry, int next) {
        if (!dragging) value = std::clamp(next, geometry.minimum, geometry.maximum);
    }
    SkinSliderEvent Down(const SkinSliderGeometry& geometry, POINT point) {
        const RECT strip = geometry.Thumb(value);
        if (PtInRect(&strip, point)) {
            captured = true;
            dragging = true;
            commit_pending = true;
            initial = value;
            grab = {point.x - strip.left, point.y - strip.top};
            return {};
        }
        // A captured slider can receive a second DOWN outside its client.
        // 004521C5 sets focus there, but does not change the value/capture.
        if (!PtInRect(&geometry.bounds, point)) return {};
        captured = dragging = true;
        value = geometry.ValueAt(point);
        // A rail press commits immediately, then continues as a centered
        // drag while the button is held. Escape restores this accepted value.
        initial = value;
        grab = {geometry.extent / 2, geometry.extent / 2};
        commit_pending = false;
        return {SB_THUMBPOSITION, true};
    }
    SkinSliderEvent Move(const SkinSliderGeometry& geometry, POINT point) {
        if (captured && dragging) {
            point.x += geometry.extent / 2 - grab.x;
            point.y += geometry.extent / 2 - grab.y;
            const int next = geometry.ValueAt(point);
            if (next == value) return {};
            value = next;
            commit_pending = true;
            return {SB_THUMBTRACK, true};
        }
        const RECT strip = geometry.Thumb(value);
        const bool inside = PtInRect(&strip, point) != FALSE;
        const bool changed = captured != inside;
        captured = inside;
        return {-1, changed};
    }
    SkinSliderEvent Up(const SkinSliderGeometry& geometry, POINT point) {
        const bool before = captured;
        const RECT strip = geometry.Thumb(value);
        point.x -= grab.x;
        point.y -= grab.y;
        if (!PtInRect(&strip, point)) captured = false;
        if (dragging) {
            const int notification = commit_pending ? SB_THUMBPOSITION : -1;
            Cancel();
            return {notification, true};
        }
        return {-1, before != captured};
    }
    SkinSliderEvent Key(const SkinSliderGeometry& geometry, UINT key) {
        if (key == VK_ESCAPE && captured) {
            Cancel();
            value = std::clamp(initial, geometry.minimum, geometry.maximum);
            return {SB_ENDSCROLL, true};
        }
        if (dragging || (geometry.vertical ? key != VK_UP && key != VK_DOWN
                                         : key != VK_LEFT && key != VK_RIGHT)) return {};
        const bool increase = key == VK_UP || key == VK_RIGHT;
        const int next = static_cast<int>(std::clamp<long long>(
            static_cast<long long>(value) + (increase ? geometry.step : -geometry.step),
            geometry.minimum, geometry.maximum));
        if (next == value) return {};
        value = next;
        return {SB_THUMBPOSITION, true};
    }
    void Cancel() { dragging = captured = commit_pending = false; }
};
} // namespace ttplayer::skin
