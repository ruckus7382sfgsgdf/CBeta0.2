"""The start-up screen's end: a fade to black of about 0.4 s before the game's first frame, none after a screen of under
1.5 s or when the window is not shown. gpu/vk/vk_main.cpp's painter draws the fade itself, so prepare_with_screen must let it
finish (paint_stop only after the wait has run out) before it hides the screen; vk_menu.cpp hands the fade to the screen's
drawing (gpu/vk/vk_startup_screen.h since the 2026-11 redesign), which draws it over everything, and puts it back to 0 when
the screen goes. After the fade the painter asks for one last black picture (fade 2) in which the screen lets its textures go.

    python -I -B tests/test_startup_fade.py
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def need(text, pattern, what):
    if not re.search(pattern, text, re.S):
        raise SystemExit("FAIL: " + what)


def main():
    main_cpp = (ROOT / "gpu/vk/vk_main.cpp").read_text(encoding="utf-8")
    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    prep = main_cpp[main_cpp.index("void prepare_with_screen() {"):main_cpp.index("bool started_up() {")]
    need(prep, r"if \(shown >= 1\.5 && !paint_stop\.load\(\)\)", "the fade is not skipped after a short screen")
    need(prep, r"/ 0\.4;.*?startup_fade_to\(\(float\)std::min\(f, 1\.0\)\).*?present_image", "the fade does not run 0.4 s and present")
    join = prep[prep.index("worker.join();"):]
    if join.index("paint_stop.store(true)") < join.index("WaitForSingleObject(painted"):
        raise SystemExit("FAIL: the painter is stopped before its fade can run")
    need(join, r"WaitForSingleObject\(painted, (\d+)\)", "no wait for the painter")
    wait = int(re.search(r"WaitForSingleObject\(painted, (\d+)\)", join).group(1))
    if wait < 1000 + 400:
        raise SystemExit("FAIL: the wait for the painter (%d ms) is shorter than a present and the fade" % wait)
    if join.index("WaitForSingleObject(painted") > join.index("startup_screen(nullptr)"):
        raise SystemExit("FAIL: the screen is hidden before the painter ends")
    draw = menu[menu.index("void draw_startup(ui::Ctx& c) {"):menu.index("void startup_fade_to(float f)")]
    need(draw, r"draw_startup_screen\(c, v, startup_fade\.load\(std::memory_order_relaxed\)\)", "the fade is not handed to the screen's drawing")
    seam = (ROOT / "gpu/vk/vk_startup_draw.h").read_text(encoding="utf-8")
    need(seam, r"start_screen::draw\(c, v, fade, start_screen::clock_now\(\)\)", "vk_startup_draw.h does not pass the fade on")
    screen = (ROOT / "gpu/vk/vk_startup_screen.h").read_text(encoding="utf-8")
    body = screen[screen.index("inline void draw(ui::Ctx& c, const StartupView& v, float fade, double now) {"):screen.index("inline double clock_now()")]
    last = body[body.rindex("AddRectFilled"):]
    need(last, r"^AddRectFilled\(ImVec2\(0, 0\), ImVec2\(w, h\), ink\(0x000000, fade\)\);", "the fade is not the last thing drawn, over the whole screen")
    need(body, r"if \(fade >= 1\.5f\) art_release\(\);", "the last picture does not let the screen's textures go")
    tail = prep[prep.index("startup_fade_to((float)std::min(f, 1.0))"):]
    need(tail, r"if \(!paint_stop\.load\(\)\) \{\s*startup_fade_to\(2\);.*?present_image.*?\}\s*SetEvent\(painted\);",
         "no last picture that lets the textures go, or one presented to a window that is not shown")
    need(menu, r"startup_on\.store\(false, std::memory_order_release\);\s*startup_fade\.store\(0", "the fade is not reset when the screen goes")
    print("PASS: the start-up screen fades to black at its end, and only after a screen worth fading")


if __name__ == "__main__":
    main()
