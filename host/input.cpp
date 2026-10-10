#include "input.h"

#include <windowsx.h>

namespace {

INPUT mkMouseBase() {
    INPUT in = {};
    in.type = INPUT_MOUSE;
    return in;
}

// Android KeyEvent keyCode -> Windows virtual key (minimal set for v0).
WORD androidToVk(uint32_t androidKey) {
    switch (androidKey) {
    case 4:  return VK_ESCAPE;      // KEYCODE_BACK
    case 3:  return VK_LWIN;        // KEYCODE_HOME -> Start (best effort)
    case 66: return VK_RETURN;      // ENTER
    case 67: return VK_BACK;        // DEL (backspace)
    case 111: return VK_ESCAPE;     // ESCAPE
    case 19: return VK_UP;
    case 20: return VK_DOWN;
    case 21: return VK_LEFT;
    case 22: return VK_RIGHT;
    case 24: return VK_VOLUME_UP;
    case 25: return VK_VOLUME_DOWN;
    case 85: return VK_MEDIA_PLAY_PAUSE;
    case 87: return VK_MEDIA_NEXT_TRACK;
    case 88: return VK_MEDIA_PREV_TRACK;
    case 61: return VK_TAB;         // TAB
    case 29: return 'Z';            // Z (ctrl combos come via meta later)
    default: return 0;
    }
}

} // namespace

void InputMapper::setDisplayRect(const RECT& rc) {
    std::lock_guard<std::mutex> lock(m_mtx);
    m_rect = rc;
}

POINT InputMapper::toScreen(float x, float y) const {
    // x,y in [0,1] relative to the display -> virtual-desktop pixels,
    // then normalized to 0..65535 for MOUSEEVENTF_ABSOLUTE|VIRTUALDESK.
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    const int vleft = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vtop = GetSystemMetrics(SM_YVIRTUALSCREEN);

    const int w = m_rect.right - m_rect.left;
    const int h = m_rect.bottom - m_rect.top;
    int px = m_rect.left + (int)(x * w + 0.5f);
    int py = m_rect.top + (int)(y * h + 0.5f);

    POINT p;
    p.x = (int)((px - vleft) * 65535.0 / (vw > 1 ? (vw - 1) : 1));
    p.y = (int)((py - vtop) * 65535.0 / (vh > 1 ? (vh - 1) : 1));
    return p;
}

void InputMapper::onTouch(const OdTouch& t) {
    std::lock_guard<std::mutex> lock(m_mtx);
    if (m_rect.right <= m_rect.left)
        return;
    // v0: only the first pointer drives the primary button; extra pointers move only.
    const bool primary = (t.pointer_id == 0);

    POINT p = toScreen(t.x, t.y);

    INPUT ins[2] = { mkMouseBase(), mkMouseBase() };
    int n = 0;

    ins[n].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    ins[n].mi.dx = p.x;
    ins[n].mi.dy = p.y;
    ++n;

    if (primary) {
        switch (t.action) {
        case OD_TOUCH_DOWN:
            ins[n].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
            ++n;
            m_pointers[t.pointer_id] = true;
            break;
        case OD_TOUCH_UP:
            ins[n].mi.dwFlags = MOUSEEVENTF_LEFTUP;
            ++n;
            m_pointers[t.pointer_id] = false;
            break;
        default:
            break; // MOVE: position only
        }
    }

    SendInput(n, ins, sizeof(INPUT));
}

void InputMapper::onKey(const OdKey& k) {
    std::lock_guard<std::mutex> lock(m_mtx);
    WORD vk = androidToVk(k.keycode);
    if (!vk)
        return;
    INPUT in = {};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.dwFlags = (k.action == 1) ? KEYEVENTF_KEYUP : 0;
    if (k.repeat)
        in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY; // crude repeat marker; Windows ignores dupes
    SendInput(1, &in, sizeof(INPUT));
}

void InputMapper::onScroll(const OdScroll& s) {
    std::lock_guard<std::mutex> lock(m_mtx);
    if (m_rect.right <= m_rect.left)
        return;
    POINT p = toScreen(s.x, s.y);

    INPUT ins[2] = { mkMouseBase(), mkMouseBase() };
    ins[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    ins[0].mi.dx = p.x;
    ins[0].mi.dy = p.y;

    ins[1].mi.dwFlags = MOUSEEVENTF_WHEEL;
    ins[1].mi.mouseData = (DWORD)(s.vscroll * WHEEL_DELTA);
    SendInput(2, ins, sizeof(INPUT));
}
