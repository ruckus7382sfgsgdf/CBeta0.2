"""The start-up screen's numbers (gpu/vk/vk_progress.h): the count with its separators, the time left from the recent rate (not
from the start, which a resumed preparation's skipped part would have made far too hopeful), and its wording. Also that a windowed
start prepares the shaders behind the screen while a headless one does not, and that a close stops the workers.

The screen's words changed with its redesign (2026-11, gpu/vk/vk_startup_screen.h): each pass is named for what it is
("Translating shaders", then "Compiling shaders" when the driver's part begins and the count starts again, where both
were "Preparing shaders" under "STEP 1 OF 2"), the steps are numbered I to III, the time left says "in this step" while
another pass follows it, and the view says how far the crest's three stones are lit.

python -I -B tests/test_startup_progress.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r'''#include "vk_progress.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
using namespace mkwvk;
int main() {
    CHECK(with_commas(0) == "0");
    CHECK(with_commas(999) == "999");
    CHECK(with_commas(1000) == "1,000");
    CHECK(with_commas(38741) == "38,741");
    CHECK(with_commas(1234567) == "1,234,567");

    CHECK(left_text(-1).empty());
    CHECK(left_text(3) == "a few seconds left");
    CHECK(left_text(36) == "about 40 s left");
    CHECK(left_text(40) == "about 40 s left");
    CHECK(left_text(58) == "about 1 min left");
    CHECK(left_text(150) == "about 3 min left");
    CHECK(version_text("v2.0.5 (source 1a2b3c4d)") == "v2.0.5");
    CHECK(version_text("v2.0.5") == "v2.0.5");
    CHECK(version_text("v2.0.5-2-g619c897-dirty (source 2c44105b)") == "v2.0.5 dev");
    CHECK(version_text("movie-sync (source 1a2b3c4d)") == "movie-sync");
    CHECK(version_text("unknown") == "unknown");
    CHECK(percent_text(0, 0).empty());
    CHECK(percent_text(12345, 38809) == "31%");
    CHECK(percent_text(38808, 38809) == "99%");   // (never 100% before the last one)
    CHECK(percent_text(38809, 38809) == "100%");

    // the screen: the crest and the wordmark alone for the first second, then the step, what it is, the count, the time left
    {
        StartupState st;
        st.secs = 0.5, st.prepared = true, st.total = 100;
        CHECK(startup_view(st).title.empty());
        st.secs = 30, st.done = 12345, st.total = 38809, st.left = 38;
        StartupView v = startup_view(st);
        CHECK(v.step == "I" && v.title == "Translating shaders" && v.pct == "31%");
        CHECK(v.left == "12,345 / 38,809 shaders" && v.right == "about 40 s left in this step" && !v.hint.empty() && !v.detail.empty());
        CHECK(v.fraction > 0.31 && v.fraction < 0.32 && !v.busy);
        CHECK(v.stones > 0.31 && v.stones < 0.32);   // (the first stone, a third of the way)
        st.pass = 1, st.compile = true;   // (the driver's part, where it is on: the count started again, and the screen says so)
        v = startup_view(st);
        CHECK(v.step == "II" && v.title == "Compiling shaders" && v.left == "12,345 / 38,809 shaders" && v.stones > 1.31 && v.stones < 1.32);
        CHECK(v.detail != startup_view(StartupState{0, 0, 12345, 38809, 38, 30, true}).detail);
        st.pass = 0, st.stage = 2, st.done = 6120, st.total = 10340, st.left = 4;
        v = startup_view(st);
        CHECK(v.step == "III" && v.title == "Building pipelines" && v.left == "6,120 / 10,340 pipelines");
        st.compile = false;   // (as things are, the driver's part off: two steps, and the pipelines are the second)
        CHECK(startup_view(st).step == "II" && startup_view(st).stones == v.stones);
        st.stage = 1, st.done = 5170;   // (half of the predicted pipelines: the second stone is due)
        CHECK(startup_view(st).stones > 1.96 && startup_view(st).stones < 1.98);
        st.compile = true;   // (with the driver's part they are the third stone alone)
        CHECK(startup_view(st).stones > 2.46 && startup_view(st).stones < 2.48);
        st.stage = 2, st.done = 6120;
        CHECK(v.right == "a few seconds left");   // (the last pass: nothing follows it)
        CHECK(v.stones > 2.97 && v.stones < 2.98);   // (the third stone: the predicted pipelines nearly all of it, these its last 6%)
        st.overdue = true;   // ("a few seconds" for too long: no time is given)
        CHECK(startup_view(st).right == "the last ones take the longest");
        st.overdue = false;
        st.stage = 1, st.total = 0;   // (the predicted pipelines, not counted yet: no figure, the line sweeps)
        v = startup_view(st);
        CHECK(v.step == "III" && v.fraction < 0 && v.busy && v.pct.empty() && v.left == "Reading the pipeline notes");
        CHECK(v.stones == 2.0);
        st.compile = false;
        CHECK(startup_view(st).step == "II" && startup_view(st).stones == 1.0);
        // a warm start: the pipelines alone, no number, no "after an update" hint; the three stones lit
        StartupState warm;
        warm.secs = 3, warm.stage = 2, warm.done = 1911, warm.total = 3822;
        v = startup_view(warm);
        CHECK(v.step.empty() && v.title == "Loading pipelines" && v.hint.empty() && v.right.empty());
        CHECK(v.stones == 3);   // (nothing left to prepare: all three lit, whatever the count)
        warm.stage = 0, warm.total = 0;   // (not known yet whether this start prepares: none)
        CHECK(startup_view(warm).stones == 0);
        warm.stage = 2, warm.total = 3822;
        warm.finished = true;
        v = startup_view(warm);
        CHECK(v.title == "Starting the game" && v.done && v.pct == "100%" && v.fraction == 1 && v.stones == 3);
        warm.secs = 0.4;   // (over before the first second: the crest alone, no flash of text)
        CHECK(startup_view(warm).title.empty() && startup_view(warm).stones == 3);
        st.stopping = true;
        v = startup_view(st);
        CHECK(v.title == "Closing" && v.closing && v.fraction < 0 && v.pct.empty() && !v.detail.empty());
    }
    // a steady 400 a second: nothing said for the first seconds nor before a sixth is done, then the rest at that rate
    ProgressClock c;
    for (int i = 0; i <= 10; i++) c.note(i * 0.25, (size_t)(i * 100), 20000);
    CHECK(c.left() < 0);
    for (int i = 11; i <= 24; i++) c.note(i * 0.25, (size_t)(i * 100), 20000);
    CHECK(c.left() < 0);   // (6 s in, 2400 of 20000: under a sixth)
    for (int i = 25; i <= 40; i++) c.note(i * 0.25, (size_t)(i * 100), 20000);
    CHECK(std::fabs(c.left() - 40) < 1);   // (10 s in, 4000 done, 16000 to go at 400/s)
    // a resumed run: 30000 of 40000 already on disk go by in two seconds, then 100 a second. The estimate soon follows the
    // slow part (the burst falls out of its window), where the average from the start would have said far less
    c.reset();
    for (int i = 0; i <= 8; i++) c.note(i * 0.25, (size_t)(i * 3750), 40000);
    double t = 2;
    size_t d = 30000;
    for (; t < 45; t += 0.5) d += 50, c.note(t, d, 40000);
    {
        const double truth = (40000.0 - d) / 100;
        CHECK(std::fabs(c.left() - truth) < truth * 0.1);
        const double from_start = (40000.0 - d) / (d / t);
        CHECK(from_start < truth / 2);   // (what the average from the start would have said)
    }
    // one slow stretch (a few heavy programs) moves what is shown a little, not five times over
    c.reset();
    t = 0, d = 0;
    for (; t < 40; t += 0.5) d += 500, c.note(t, d, 100000);
    const double before = c.left();
    for (int k = 0; k < 16; k++) t += 0.5, d += 20, c.note(t, d, 100000);
    CHECK(c.estimate(100000) > before * 1.8);   // (the unsmoothed rate did jump)
    CHECK(c.left() < c.estimate(100000) * 0.85);
    // a pass whose last few are slow: the recent rate says "a few seconds" on and on. After ten seconds of that it is overdue
    {
        ProgressClock o;
        double ot = 0;
        size_t od = 0;
        for (int k = 0; k < 40; k++) ot += 0.5, od += 250, o.note(ot, od, 10060);
        CHECK(o.left() >= 0 && o.left() < 10 && !o.overdue(ot));
        for (int k = 0; k < 24; k++) ot += 0.5, od += 1 + (k & 1), o.note(ot, od, 10060);
        CHECK(od < 10060 && o.left() >= 0 && o.left() < 10);   // (still promising)
        CHECK(o.overdue(ot));
        o.reset();
        CHECK(!o.overdue(ot));
    }
    // done: nothing left; a new stage (the count going back) starts over
    c.note(t + 1, 100000, 100000);
    CHECK(c.left() == 0);
    c.note(t + 1.5, 10, 4000);
    CHECK(c.left() < 0);
    // samples closer than a quarter second are dropped, so a fast caller does not fill the window with one instant
    c.reset();
    for (int i = 0; i < 1000; i++) c.note(i * 0.001, (size_t)i, 2000);
    CHECK(c.n <= 4);
    // nothing moving for a while (a driver busy on one pipeline): no estimate rather than a division by zero
    c.reset();
    for (int i = 0; i < 40; i++) c.note(i * 0.5, 4570, 4571);
    CHECK(c.left() < 0);
    std::printf("PASS: counts, time left and its wording\n");
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()

    main_cpp = (ROOT / "gpu" / "vk" / "vk_main.cpp").read_text(encoding="utf-8")
    draw = (ROOT / "gpu" / "vk" / "vk_draw.cpp").read_text(encoding="utf-8")
    started = main_cpp[main_cpp.index("bool started_up() {"):main_cpp.index("int start(void* hwnd) {")]
    if "if (!dev.offscreen && screen_wanted())" not in started:
        raise SystemExit("a windowed start no longer prepares the shaders behind the start-up screen")
    if started.index("screen_wanted()") > started.index("if (console) CloseHandle(console)"):
        raise SystemExit("the start-up screen is chosen after the headless preparation")
    screen = main_cpp[main_cpp.index("void prepare_with_screen() {"):main_cpp.index("bool started_up() {")]
    for need, why in (("std::thread worker(", "the preparation runs on the window's own thread"),
                      ("std::thread painter(", "a present that waits for a hidden window holds the window's messages or the game"),
                      ("present_image(nullptr", "nothing is shown while it runs"),
                      ("PeekMessageA", "the Windows window's messages wait meanwhile (not responding)"),
                      ("dev.swap_resize.store", "a resize while it runs is not taken")):
        if need not in screen:
            raise SystemExit(why)
    if 'void mkwvk_close_asked(void)' not in main_cpp or "prepare_stop();" not in main_cpp:
        raise SystemExit("closing the window no longer stops the preparation")
    for fn in ("bool prepare_run(", "bool warm_libraries(", "void predict_run(", "void precompile_run("):
        body = draw[draw.index(fn):]
        body = body[:body.index("\n}\n")]
        if body.count("prepare_stopped()") < 2:
            raise SystemExit(fn + " does not stop its workers and its wait when the window is closed")
    prep = draw[draw.index("bool prepare_run("):]
    prep = prep[:prep.index("\n}\n")]
    stop_at = prep.index("if (prepare_stopped()) {")
    if prep.index('prepared_write(', stop_at) < prep.index("return false;", stop_at):
        raise SystemExit("a stopped preparation writes the stamp that says it is all done")
    for host in ("host/display.c", "host/posix/display_sdl.c"):
        text = (ROOT / host).read_text(encoding="utf-8")
        if text.count("rx_close_asked();") < 1:
            raise SystemExit(host + " ends the process on close without letting the preparation keep its work")

    with tempfile.TemporaryDirectory(prefix="startup-progress-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture"
        cpp.write_text(FIXTURE, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "gpu" / "vk"), str(cpp), "-o", str(exe)],
                       check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
