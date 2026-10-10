#pragma once
#include <windows.h>
#include <mutex>
#include <unordered_map>

#include "../proto/opendisplay_proto.h"

// Maps phone touch/key events (relative to the virtual display) to
// Windows SendInput absolute mouse/keyboard events.
class InputMapper {
public:
    // Desktop rectangle of the target display in virtual-screen coordinates.
    void setDisplayRect(const RECT& rc);

    void onTouch(const OdTouch& t);
    void onKey(const OdKey& k);
    void onScroll(const OdScroll& s);

private:
    POINT toScreen(float x, float y) const;

    RECT m_rect = {};
    std::mutex m_mtx;
    bool m_primaryDown = false;
    // pointer_id -> currently pressed (for correct UP without MOVE)
    std::unordered_map<uint32_t, bool> m_pointers;
};
