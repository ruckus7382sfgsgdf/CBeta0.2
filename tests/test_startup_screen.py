"""The start-up screen's drawing (gpu/vk/vk_startup_screen.h) with no game and no GPU: tests/startup_screen_preview.cpp plays
a first start, an ordinary start, a start closed part-way and a start with nothing to prepare through the screen's own
code, sixty pictures a second, into Dear ImGui's draw lists, and rasterizes the pictures of each state. Checked there: no
picture puts a corner far outside the window, and the last picture lets the screen's textures go while drawing none of them.
Checked here: every picture is written and is neither empty nor one flat colour, the stones are lit where the state says
(dark glass before, the stone's own colour after), and the fade's last picture is black.

Also what the game's start depends on in the files around it: vk_menu.cpp hands the view, the fade and a clock to the
screen, and vk_main.cpp gives the screen the pass the preparation is in (vk_draw.cpp prepare_pass), so that the driver's
part of the shaders is not shown under the translation's name.

    python -I -B tests/test_startup_screen.py [--cxx clang++] [--headers <the Vulkan headers' include folder>]
"""
import argparse
import importlib.util
import struct
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]

ROOT = Path(__file__).resolve().parents[1]
W, H = 960, 540


def picture(path):
    data = path.read_bytes()
    w, h = struct.unpack_from("<II", data, 0)
    if (w, h) != (W, H) or len(data) != 8 + w * h * 4:
        raise SystemExit("FAIL: %s is not a %dx%d picture" % (path.name, W, H))
    return data[8:]


def mean(px, box):
    """The mean colour of a box (x0, y0, x1, y1), in shares of the window"""
    x0, y0, x1, y1 = int(box[0] * W), int(box[1] * H), int(box[2] * W), int(box[3] * H)
    total, n = [0, 0, 0], 0
    for y in range(y0, y1):
        row = px[(y * W + x0) * 4:(y * W + x1) * 4]
        for k in range(3):
            total[k] += sum(row[k::4])
        n += x1 - x0
    return [t / n for t in total]


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--cxx", default="clang++")
    p.add_argument("--headers", default="")
    a = p.parse_args()

    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    main_cpp = (ROOT / "gpu/vk/vk_main.cpp").read_text(encoding="utf-8")
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    hook = menu[menu.index("void draw_startup(ui::Ctx& c) {"):menu.index("void startup_fade_to(float f)")]
    if "draw_startup_screen(c, v, startup_fade.load(std::memory_order_relaxed));" not in hook:
        raise SystemExit("FAIL: vk_menu.cpp's draw_startup does not draw the screen of vk_startup_screen.h")
    if '"HOOKSHOT"' in hook:
        raise SystemExit("FAIL: the start-up screen still says HOOKSHOT")
    painter = main_cpp[main_cpp.index("void prepare_with_screen() {"):main_cpp.index("bool started_up() {")]
    for need, why in (("const int pass = stage == 0 ? prepare_pass() : 0;", "the screen is not told which pass the shaders are in"),
                      ("st.pass = pass", "the pass does not reach the screen's state"),
                      ("stage * 2 + pass != eta_stage", "the time left is not started again when the driver's part begins"),
                      ("view.device = dev.props.deviceName;", "the graphics card's name does not reach the screen")):
        if need not in painter:
            raise SystemExit("FAIL: " + why)
    warm = draw[draw.index("bool warm_libraries("):]
    warm = warm[:warm.index("\n}\n")]
    prep = draw[draw.index("bool prepare_run("):]
    prep = prep[:prep.index("\n}\n")]
    if "prepare_pass_now.store(1" not in warm or "prepare_pass_now.store(0" not in prep:
        raise SystemExit("FAIL: vk_draw.cpp does not say when the driver's part of the shaders begins")

    spec = importlib.util.spec_from_file_location("startup_screen_preview", ROOT / "tests" / "startup_screen_preview.py")
    preview = importlib.util.module_from_spec(spec)
    sys.path.insert(0, str(ROOT / "nx2"))
    spec.loader.exec_module(preview)
    import subprocess
    with tempfile.TemporaryDirectory(prefix="startup-screen-") as tmp:
        tmp = Path(tmp)
        exe = preview.build(a.cxx, tmp / "build", a.headers, "-O1")
        out = tmp / "out"
        out.mkdir()
        # (the bundled font: no game files here)
        subprocess.run([str(exe), str(out), "-", str(ROOT / "third_party" / "fonts" / "libertinus"), "--size", "%dx%d" % (W, H)], check=True)
        names = sorted(f.stem for f in out.glob("*.rgba"))
        if len(names) < 12:
            raise SystemExit("FAIL: %d pictures, 12 states and more are expected" % len(names))
        pics = {n: picture(out / (n + ".rgba")) for n in names}
    for name, px in pics.items():
        if name == "08-fade":
            continue
        colours = len(set(px[i:i + 3] for i in range(0, len(px), 4 * 97)))
        if colours < 24:
            raise SystemExit("FAIL: %s is flat (%d colours sampled)" % (name, colours))
    # the stones, where vk_startup_screen.h puts them: each one's box in the crest (stone_masks' boxes, of the 512 x 468
    # crest), the crest's box from the layout. Unlit a stone is dark glass with next to no colour; lit, a good share of its
    # box is strongly its own colour
    def stone(px, k):
        cw, ch = 180 * H / 640 * 512 / 468, 180 * H / 640
        top = int((H - 390 * H / 640) / 2 - 18 * H / 640)
        box = ((190, 80, 340, 225), (265, 225, 460, 410), (80, 225, 225, 370))[k]
        lead = (1, 2, 0)[k]   # (emerald green, sapphire blue, ruby red)
        x0, y0 = int(W / 2 - cw / 2 + cw * box[0] / 512), int(top + ch * box[1] / 468)
        x1, y1 = int(W / 2 - cw / 2 + cw * box[2] / 512), int(top + ch * box[3] / 468)
        strong = 0
        for y in range(y0, y1):
            for x in range(x0, x1):
                c = px[(y * W + x) * 4:(y * W + x) * 4 + 3]
                # (the gold round the ruby is red-led too, by less)
                strong += c[lead] > 90 and c[lead] > (2.4 if k == 2 else 1.6) * max(c[i] for i in range(3) if i != lead)
        return strong / ((x1 - x0) * (y1 - y0))
    for name, lit in (("03-translating", ()), ("05-building-pipelines", (0,)), ("06-second-stone-ignites", (0, 1)),
                      ("07-starting-the-game", (0, 1, 2)), ("13-with-the-drivers-part", (0,)),
                      ("09-warm-start", (0, 1, 2)), ("09b-warm-start-stones-lit", (0, 1, 2)), ("10-warm-start-done", (0, 1, 2)),
                      ("11-closing", ())):
        for k in range(3):
            share = stone(pics[name], k)
            if (share > 0.08) != (k in lit):
                raise SystemExit("FAIL: %s: stone %d is %s (%.0f%% of its box in its colour)"
                                 % (name, k, "lit" if share > 0.08 else "unlit", share * 100))
    if max(mean(pics["08-fade"], (0, 0, 1, 1))) > 40:
        raise SystemExit("FAIL: the fade does not darken the screen")
    print("PASS: the start-up screen draws every state: %d pictures, the stones lit as the preparation goes" % len(pics))


if __name__ == "__main__":
    main()
