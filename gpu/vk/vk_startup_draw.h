// (triforce) The seam between the start-up screen's drawing (vk_startup_screen.h, its own session's file) and vk_menu.cpp's
// draw_startup, which calls this and nothing else of it. fade: startup_fade as it is (0 to 1 darkens the screen to black at
// its end; 2 is its last picture, in which its textures are let go).
#pragma once

#include "vk_startup_screen.h"

namespace mkwvk {

inline void draw_startup_screen(ui::Ctx& c, const StartupView& v, float fade) {
    start_screen::draw(c, v, fade, start_screen::clock_now());
}

} // namespace mkwvk