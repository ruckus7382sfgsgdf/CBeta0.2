// (hookshot) The start-up screen's numbers (vk_main.cpp, vk_menu.cpp): how far the shaders' preparation is, and about how long
// it has left. Plain C++, no Vulkan: tests/test_startup_progress.py compiles it on its own.
#pragma once

#include <cstddef>
#include <cstdio>
#include <algorithm>
#include <string>

namespace mkwvk {

/// The time left, from the rate over the last few seconds, not from the start: a start after one that was closed part-way skips
/// what is on disk already in a second or two, and that burst would have promised the rest at the same speed. Nothing is said
/// before a sixth is done (the programs differ a lot: on an RTX 3090 the first 1,500 of 32,000 took half a minute, the next 10,000
/// ten seconds, and the early estimates said 83 minutes for a 70-second job), and what is said is smoothed over about 8 s
/// (it went 1 min, 5 min, 1 min from one 10-second line to the next)
struct ProgressClock {
    static constexpr int kSamples = 64;
    static constexpr double kWindow = 15.0;    // (seconds the rate is taken over)
    static constexpr double kFirst = 3.0;      // (seconds of samples before any estimate)
    static constexpr double kMinShare = 1.0 / 6;
    static constexpr double kSmooth = 8.0;     // (seconds: the estimate's time constant)
    double t[kSamples]{};
    size_t done[kSamples]{};
    int n = 0, head = 0;   // (the samples, oldest at head)
    double last_note = -1, shown = -1;
    double under_at = -1;   // (since when the time left shown has been under ten seconds)

    void reset() { n = head = 0, last_note = shown = -1, under_at = -1; }
    /// A sample: at seconds t_now, done of total (four a second at most are kept)
    void note(double t_now, size_t done_now, size_t total) {
        if (n && done_now < done[(head + n - 1) % kSamples]) reset();   // (a new stage)
        if (last_note >= 0 && t_now - last_note < 0.25) return;
        const double dt = last_note < 0 ? 0 : t_now - last_note;
        last_note = t_now;
        if (n == kSamples) head = (head + 1) % kSamples, n--;
        t[(head + n) % kSamples] = t_now, done[(head + n) % kSamples] = done_now, n++;
        while (n > 2 && t_now - t[(head + 1) % kSamples] >= kWindow) head = (head + 1) % kSamples, n--;
        const double raw = estimate(total);
        if (raw < 0) shown = -1;
        else if (shown < 0 || raw == 0) shown = raw;   // (the first one, or all done)
        else {
            shown = shown > dt ? shown - dt : 0;   // (time passing)
            shown += (raw - shown) * (dt < kSmooth ? dt / kSmooth : 1.0);
        }
        if (shown < 0 || shown >= 10) under_at = -1;
        else if (under_at < 0) under_at = t_now;
    }
    /// "A few seconds left" has been the answer for over ten seconds: it was not so. The last of a pass are its heaviest, and
    /// fewer and fewer workers have one left, so the recent rate keeps promising an end that does not come (measured: the
    /// predicted pipelines said "a few seconds left" for their last 24 s). No time is given from then on
    bool overdue(double t_now) const { return under_at >= 0 && t_now - under_at > 10; }
    /// Seconds left, or a negative number when it cannot be said yet
    double left() const { return shown; }
    /// The rate over the window alone, unsmoothed
    double estimate(size_t total) const {
        if (n < 2) return -1;
        const int last = (head + n - 1) % kSamples;
        const double span = t[last] - t[head];
        if (done[last] >= total) return 0;
        if (span < kFirst || done[last] <= done[head] || (double)done[last] < (double)total * kMinShare) return -1;
        const double rate = (double)(done[last] - done[head]) / span;
        return (double)(total - done[last]) / rate;
    }
};

/// 12345 as "12,345"
inline std::string with_commas(size_t v) {
    std::string s = std::to_string(v);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert((size_t)i, ",");
    return s;
}

/// "about 40 s left": up to 5 s under a minute, whole minutes above; empty when unknown
inline std::string left_text(double secs) {
    if (secs < 0) return "";
    char b[48];
    if (secs < 10) return "a few seconds left";
    if (secs < 55) snprintf(b, sizeof b, "about %d s left", (int)((secs + 4.999) / 5) * 5);
    else snprintf(b, sizeof b, "about %d min left", secs < 90 ? 1 : (int)(secs / 60 + 0.5));
    return b;
}

/// The version line from the build's name (nx2/version.py, "v2.0.5 (source 1a2b3c4d)"): the release alone, "v2.0.5"; a build
/// between releases ("v2.0.5-2-g619c897-dirty", git describe) shows as "v2.0.5 dev"; anything else as it is
inline std::string version_text(std::string name) {
    if (const size_t at = name.find(" ("); at != std::string::npos) name.resize(at);
    size_t i = 0;
    if (name.size() > 1 && name[0] == 'v') {
        int dots = 0;
        for (i = 1; i < name.size() && ((name[i] >= '0' && name[i] <= '9') || name[i] == '.'); i++) dots += name[i] == '.';
        if (dots == 2 && i < name.size() && name[i] == '-') return name.substr(0, i) + " dev";
    }
    return name;
}

/// "32%": rounded down, and 99% until all is done
inline std::string percent_text(size_t done, size_t total) {
    if (!total) return "";
    size_t p = done >= total ? 100 : done * 100 / total;
    if (p > 99 && done < total) p = 99;
    return std::to_string(p) + "%";
}

/// What the start-up screen shows (vk_startup_screen.h draws it). The preparation of a first start is two steps, the shaders
/// translated and the pipelines built, or three where the graphics driver's part of the shaders is switched on between
/// them; the crest's three stones light as it goes (stones_lit)
struct StartupView {
    std::string version;            // ("v2.0.5"; empty: the build's)
    std::string step, title, pct;   // ("II", "Compiling shaders", "32%"); an empty title: the crest and the wordmark alone
    std::string left, right;        // (the detail beside the percentage: "12,345 / 38,809 shaders", "about 40 s left in this step")
    std::string detail;             // (one sentence under the line: what this step is)
    std::string hint;               // (one line at the foot)
    std::string device;             // (the graphics card, in a corner)
    double fraction = -1;           // (the line, 0 to 1; negative: none)
    double stones = 0;              // (0 to 3: how far the crest's three stones are lit)
    bool busy = false;              // (working, nothing counted yet: a piece of the line sweeps)
    bool done = false, closing = false;
    bool linear = false;            // (the window's images blend in linear light: the lights' alphas are made to match)
    bool operator==(const StartupView&) const = default;
};

/// The state of the start: stage 0 the shader programs, 1 the pipelines the notes predict, 2 the noted pipelines; pass, in
/// stage 0: 0 the translation, 1 the driver's part (vk_draw.cpp prepare_pass); done of total in it (total 0: not counted
/// yet); left the seconds left (negative: unknown); prepared: this start prepares programs (a first start after an update:
/// three parts, else the pipelines alone); secs since the window opened
struct StartupState {
    int stage = 0, pass = 0;
    size_t done = 0, total = 0;
    double left = -1, secs = 0;
    bool prepared = false, finished = false, stopping = false;
    bool overdue = false;   // (the time left was "a few seconds" for too long: ProgressClock::overdue)
    bool compile = false;   // (the preparation has the driver's part of the shaders: vk_draw.cpp prepare_compiles)
};

/// How far the stones are lit, 0 to 3. A first start with the driver's part: a stone a step. Without it (as things are: that
/// part is off unless switched on) there are two steps: the first stone is the translation, the other two the halves of the
/// pipelines. Any other start has nothing left to prepare, its pipelines are read back: all three are lit
/// as soon as it is known to be one (the worker is at the pipelines with no shader prepared). They were its thirds at first:
/// measured, the count of such a start goes by in a fifth of a second, after two seconds of opening the caches, and the
/// stones then lit only as the screen faded
inline double stones_lit(const StartupState& s) {
    if (s.finished) return 3;
    const double f = s.total ? (double)std::min(s.done, s.total) / (double)s.total : 0;
    if (!s.prepared) return s.stage == 2 ? 3 : 0;
    if (s.stage == 0) return (s.pass ? 1 : 0) + f;
    // (the noted pipelines of a start that prepared are few, a fifth of a second measured, after 6 to 44 s of predicted ones)
    if (s.stage == 2) return 2.94 + f * 0.06;
    return s.compile ? 2 + f * 0.94 : 1 + f * 1.94;
}

/// The screen for a state. The first second shows the crest and the wordmark alone: a start with next to nothing to prepare
/// is over by then
inline StartupView startup_view(const StartupState& s) {
    StartupView v;
    v.stones = stones_lit(s);
    if (s.stopping) {
        v.closing = true;
        v.title = "Closing";
        v.left = "Keeping what is prepared";
        v.detail = "The next start goes on from here.";
        return v;
    }
    if (s.finished) {
        v.done = true;
        if (s.secs >= 1.0) v.title = "Starting the game", v.pct = "100%", v.fraction = 1;
        return v;
    }
    if (s.secs < 1.0) return v;
    const bool pipelines = s.stage > 0, compile = !pipelines && s.pass > 0;
    if (s.prepared) v.step = !pipelines ? (compile ? "II" : "I") : s.compile ? "III" : "II";
    v.title = pipelines ? (s.prepared ? "Building pipelines" : "Loading pipelines") : compile ? "Compiling shaders" : "Translating shaders";
    v.detail = pipelines ? (!s.prepared ? "What earlier starts prepared is read back."
                            : s.stage == 1 ? "The shaders are joined the way the game is expected to draw."
                                           : "The shaders are joined the way the game has drawn before.")
               : compile ? "The graphics driver compiles each shader and keeps the result."
                         : "The game's shaders are rewritten for this PC's graphics card.";
    if (s.prepared) v.hint = "This happens once after an update. Closing the window keeps what is done.";
    if (!s.total) {
        v.busy = true;
        v.left = pipelines ? (s.stage == 1 ? "Reading the pipeline notes" : "Opening the shader cache") : compile ? "Opening the shader cache" : "Reading the game's shaders";
        return v;
    }
    v.fraction = (double)std::min(s.done, s.total) / (double)s.total;
    v.pct = percent_text(s.done, s.total);
    v.left = with_commas(s.done) + " / " + with_commas(s.total) + (pipelines ? " pipelines" : " shaders");
    // (the time left is this pass's alone: said so while another one follows it)
    v.right = left_text(s.left);
    if (s.overdue) v.right = "the last ones take the longest";
    else if (!v.right.empty() && s.prepared && s.stage < 2) v.right += " in this step";
    return v;
}

} // namespace mkwvk