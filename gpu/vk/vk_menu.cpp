// The Insert menu. Drawn with Dear ImGui over the window's image at the last step of a present (menu_record; vk_imgui.h, in the
// game's style: vk_ui.h), so it costs nothing while closed and it never touches the game's picture, nor what frame interpolation
// sees. The FPS counter and the start-up screen are drawn the same way. F1 always opens and closes it, Insert too by default.
// Up and Down choose, Left and Right change, Esc closes; the game gets no keyboard input while it is open.
// (hookshot) A gamepad's Home button, or Select and Start together, opens it too; the D-pad chooses and changes, the button
// the Switch's A is bound to changes too (and opens a page), the one B is bound to goes back (vk_menu_keys.h). The game gets no gamepad input while it is open either.
// Its Controls page binds the Switch's buttons to gamepad inputs and keys.
// (hookshot) The mouse is free while it is open (the window lets it go: WM_APP + 8, host/display.c and
// host/posix/display_sdl.c): the row under the cursor is selected, a click on a tab shows it, on a value's left or right
// half changes it down or up, on a toggle flips it, on a slider's track sets it (or drag it); the wheel scrolls the rows
// (never a value, wherever the cursor is); a right click is Esc. On the Cheats page and its lists a click is Enter (on a
// choice's or an amount's left or right half, Left or Right) and a right click goes back. The drawing records each control's
// place and the mouse is tested against that (vk_menu_hit.h).
// (hookshot) The rows that take a restart (Row::restart) are tagged "After restart" while their value is not the one the game
// started with, and the Graphics tab's help lists them. Closing the menu then asks whether to restart now (menu_restart.h):
// Later keeps the saved settings for the next start and is not asked again until they change; Restart now saves and
// closes the window as the player would, and the game starts again (WM_APP + 9: host/hos_relaunch.c).
// (Triforce, 11-10) The menu is drawn as the game's own Options screen (SystemOption_00 of its layout archive): the System
// screens' header with the options' categories across it as the pause menu has its own (Display, Graphics, Audio, Controls,
// System; L and R, Q and E), the rows on the game's plates in its list column, the caption plate at the right, the guides at
// the foot, in the game's fonts and with its button icons (vk_ui.h, vk_game_tex.h, vk_imgui.cpp: all read from the player's
// game files at run time). The Debug tab is not shown until Alt+G unlocks it (debug_tab=1 in the ini; Alt+G again puts it
// away and its tools off). The Menu size row is gone: the game's menus have one size. The FPS counter is a HUD counter (the
// HUD's own digits, no plate). tests/ui_preview.cpp draws all of it into pictures without the game.
#include <atomic>
#include <chrono>
#include <cmath>
#include <utility>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#include "vk_paths.h"
#include "vk_menu.h"
#include "vk_menu_hit.h"
#include "vk_draw.h"   // (pipeline_cache_save: Restart now keeps what was built)
#include "oot_cheat_items.h"
#include "vk_progress.h"   // (StartupView, version_text: the start-up screen)
#if __has_include("vk_startup_draw.h")   // (the shader-screen branch's file: with it the start-up screen is its drawing)
#include "vk_startup_draw.h"   // (the start-up screen's drawing: draw_startup_screen)
#endif
#include "vk_ui.h"
#include "vk_menu_keys.h"   // (which gamepad buttons confirm and go back: those A and B are bound to)         // (how it looks: Dear ImGui, vk_imgui.h)
#include "vk_dlss.h"
#include "vk_fsr3.h"      // (the Upscaler row: whether AMD FSR 3.1 runs on this GPU)
#include "vk_fsr3fi.h"    // (the Frame generation row: whether FSR 3.1 makes the frame)
#include "vk_fsr4.h"
#include "vk_temporal.h"
#include "vk_renodx.h"    // (renodx_built: the RenoDX rows only in a build with its shaders)
#include "vk_reflex.h"
#include "vk_antilag.h"
#include "upscaler_rows.h"
#include "menu_restart.h"
#include "vk_work.h"   // (after the others: counts the commands that do GPU work, for full_barrier)
#ifndef HOOKSHOT_VERSION   // (nx2/build.py gives it to this file: nx2/version.py)
#define HOOKSHOT_VERSION "unknown"
#endif

namespace mkwvk {
HWND app_window();

Options opts;

namespace {

// (Triforce) The tabs across the Options screen's header, as the pause menu's categories: a kind of option each. Debug is the
// last and is not shown until Alt+G unlocks it (debug_shown)
enum MenuTab {
    TAB_DISPLAY = 0,
    TAB_GRAPHICS = 1,
    TAB_AUDIO = 2,
    TAB_CONTROLS = 3,
    TAB_SYSTEM = 4,
    TAB_DEBUG = 5,   // (the OoT debug tools of #785 and the developer's rows, a tab of their own)
    TAB_COUNT = 6,
    TAB_MOTION = TAB_COUNT,   // (hookshot: not a tab of the strip, the Motion page that the Controls tab's Motion row
                              // opens: one past the strip's tabs, so a tab added before TAB_COUNT moves it along)
    TAB_PAGE = TAB_MOTION + 1   // (hookshot: the Controls page's rows, above its grid; no tab shows them)
};

struct Row {
    MenuTab tab;
    const char* name;
    int* value;
    int lo, hi, step;
    const char* (*text)(int v);
    const char* help;   // (shown under the list while the row is selected: what the row is, the same whatever its value)
    // (a line about the value chosen, under the help, or nullptr: hint(v, -1) is the one to show now for v, hint(v, 0),
    // hint(v, 1), ... every one v can have (those that depend on the other rows or on the GPU), until nullptr, so the
    // menu keeps the height of the tallest: measure_help)
    const char* (*hint)(int v, int variant) = nullptr;
    bool restart = false;   // (read once, at the start: a change applies after a restart)
    bool preset_exempt = false;   // (a change here leaves the Quality preset alone: the controls, the window, the counter,
                                  // frame generation)
    signed char cheat = -1;   // (a debug tool, on the Debug tab with the tools on: its index in the cheat_ tables, -1 none)
    bool heading = false;     // (a section's title on its tab, not a row: the selection steps over it)
};
/// A row's hints from a table, the value lo first (nullptr: none for that value)
const char* hint_in(const char* const* t, int n, int lo, int v, int variant) {
    return variant <= 0 && v - lo >= 0 && v - lo < n ? t[v - lo] : nullptr;
}
#define MENU_HINTS(fn, lo, ...)                                                                                              \
    const char* fn(int v, int variant) {                                                                                     \
        static const char* const t[] = {__VA_ARGS__};                                                                        \
        return hint_in(t, (int)(sizeof t / sizeof t[0]), lo, v, variant);                                                    \
    }
const char* tx_framegen(int v) {
    static const char* t[] = {"Off", "Auto", "2x", "3x", "4x", "5x", "6x", "7x", "8x"};
    return t[v];
}
const char* tx_onoff(int v) { return v ? "On" : "Off"; }
const char* tx_menu_shortcut(int v) {
    static const char* const names[] = {"Insert or F1", "F1 only"};   // (F1 is never turned off: the menu is always one key away)
    return names[v ? 1 : 0];
}
const char* tx_hdr(int v) { return v == 1 ? "Expanded" : v == 2 ? "Native (the game's)" : "Off"; }
const char* tx_nits(int v) {
    static char b[24];
    snprintf(b, sizeof b, "%d nits", v);
    return b;
}
const char* tx_fps_cap(int v) {
    static const char* const t[] = {"60 (the console's)", "90", "120", "144", "165", "240"};
    return t[v < 0 ? 0 : v > 5 ? 5 : v];
}
// (hookshot) Motion controls: the choice, and whether a connected controller has a gyroscope (host/pad_sdl.c). Either can be
// chosen whatever is connected: a pad plugged in later is read when it comes
const char* tx_gyro(int v) {
    const int status = host && host->gyro_status ? host->gyro_status() : -1;
    if (status == 2) return v ? "On (gyro found)" : "Off (gyro found)";
    if (status == 1) return v ? "On (no gyro on this pad)" : "Off (no gyro on this pad)";
    if (status == 0) return v ? "On (no controller)" : "Off (no controller)";
    return v ? "On" : "Off";
}
const char* tx_mousecam(int v) { return v ? "Native" : "As the stick"; }
const char* tx_sens(int v) {
    static char b[12];
    snprintf(b, sizeof b, "%.2f", v / 100.0);
    return b;
}
// (the game's picture and the window it is shown in, as the last present had them: menu_note_present_sizes)
std::atomic<uint64_t> present_sizes{0};
bool filter_needed() {
    const uint64_t v = present_sizes.load(std::memory_order_relaxed);
    return upscaler_rows::filter_needed((unsigned)(v >> 48), (unsigned)(v >> 32) & 0xffff, (unsigned)(v >> 16) & 0xffff,
                                        (unsigned)v & 0xffff);
}
const char* tx_upscale(int v) {
    static const char* t[] = {"Off", "Catmull-Rom", "Lanczos", "AMD FSR 1"};
    if ((unsigned)v >= sizeof t / sizeof t[0]) return t[0];   // (a setting past the row: the label, not the next bytes)
    return t[v];
}
/// The Window scaling filter row's value: its filter, or that none is needed (the saved value stays, for another window size)
const char* tx_filter(int v) { return filter_needed() ? tx_upscale(v) : "Not needed (window = output)"; }
const char* tx_reflex(int v) {
    static const char* t[] = {"Off", "On", "On + Boost"};
    return t[reflex_available() ? std::clamp(v, 0, 2) : 0];
}
/// Reflex: on the value, or that it is not available here (the row shows Off)
const char* hn_reflex(int v, int variant) {
    static const char* const t[] = {"The game as without it.", "The driver paces the frames for less latency.",
                                    "As On, and the GPU's clocks are kept up as well."};
    static const char* const none = "Not available here: it needs an NVIDIA GPU whose driver has VK_NV_low_latency2.";
    if (variant < 0) return reflex_available() ? t[std::clamp(v, 0, 2)] : none;
    return variant == 0 ? t[std::clamp(v, 0, 2)] : variant == 1 ? none : nullptr;
}
const char* tx_antilag(int v) { return antilag_available() && v ? "On" : "Off"; }
/// Anti-Lag 2: on the value, or that it is not available here (the row shows Off)
const char* hn_antilag(int v, int variant) {
    static const char* const t[] = {"The game as without it.", "The driver delays each frame's start for less latency."};
    static const char* const none = "Not available here: it needs an AMD GPU whose driver has VK_AMD_anti_lag.";
    if (variant < 0) return antilag_available() ? t[v ? 1 : 0] : none;
    return variant == 0 ? t[v ? 1 : 0] : variant == 1 ? none : nullptr;
}
// The Upscaler (opts.dlss): what scales the 3D up to the output. DLSS and DLAA only where dlss_why() lets them run, FSR 3.1
// only where vk_fsr3.cpp says it does, FSR 4 and its native AA only where fsr4_why() does; the menu shows (and steps from)
// what runs, and keeps the saved choice for a PC where it can
namespace ur = upscaler_rows;
/// One bit per upscaler that runs here (upscaler_rows.h bit). Each backend caches its own answer, so this costs nothing
unsigned upscalers_available() {
    return ur::bit(ur::GAME) | ur::dlss_bits(dlss_why() == DLSS_OK) | (fsr3::supported() ? ur::bit(ur::FSR3) : 0u) |
           ur::fsr4_bits(fsr4_why() == FSR_OK);
}
/// (FSR 3.1 has inputs of its own; DLSS and FSR 4 run on the frame chain they share, temporal_mode)
int upscaler() { return fsr3_wanted() ? (int)ur::FSR3 : temporal_mode(); }
const char* tx_upscaler(int) {
    static const char* t[ur::kUpscalers] = {"Game's own", "NVIDIA DLSS", "NVIDIA DLAA", "AMD FSR 3.1", "AMD FSR 4",
                                            "AMD FSR 4 Native AA"};
    return t[std::clamp(upscaler(), 0, ur::kUpscalers - 1)];
}
/// The Upscaler: the one shown (what runs, upscaler()), and with the game's own, why DLSS and FSR are not offered. Variants
/// (every one): each value, then the game's own with each of DLSS's reasons, then with each of FSR's
const char* hn_upscaler(int v, int variant) {
    static const char* const t[ur::kUpscalers] = {
        "The game's own network.",
        "NVIDIA's, from a smaller 3D. Softer than the game's own: set Sharpness to around 80%. Things that move by "
        "themselves can trail for now.",
        "NVIDIA's, the 3D at full size with its edges smoothed; the slowest. Softer than the game's own: set Sharpness to "
        "around 80%. Things that move by themselves can trail for now.",
        "AMD's, from a smaller 3D, on any GPU: built from AMD's own shaders. Things that move by themselves can trail for "
        "now.",
        "AMD's machine-learned upscaler, from a smaller 3D, with AMD's files beside the game. Things that move by themselves "
        "can trail for now.",
        "AMD's machine-learned upscaler, the 3D at full size with its edges smoothed; the slowest.",
    };
    static const char* const dlss_no[DLSS_WHY_COUNT] = {
        nullptr,
        "DLSS and DLAA: not in this build (built without NVIDIA's DLSS SDK, see the readme).",
        "DLSS and DLAA: off for this run (RX_VK_DLSS=0).",
        "DLSS and DLAA: need an NVIDIA RTX GPU.",
        "DLSS and DLAA: need a newer NVIDIA driver.",
        "DLSS and DLAA: DLSS's runtime was not found (the game folder's ngx folder, see the readme).",
        "DLSS and DLAA: NVIDIA's NGX on Linux loads DLSS's runtime only from a folder named in ASCII (see the readme).",
        "DLSS and DLAA: NVIDIA's NGX started but gave DLSS nothing to work with (see the log).",
    };
    static const char* const fsr_no[FSR_WHY_COUNT] = {
        nullptr,
        "AMD FSR 4: not in this build (see the readme).",
        "AMD FSR 4: off for this run (RX_VK_FSR4=0).",
        "AMD FSR 4: AMD's files were not found beside the game (see the readme).",
        "AMD FSR 4: AMD's files have no FSR 4 for this GPU.",
    };
    // (now: the value shown and what runs here, with both engines' reasons; every one: as said above)
    int d = 0, f = 0;
    if (variant < 0) {
        v = std::clamp(upscaler(), 0, ur::kUpscalers - 1), d = (int)dlss_why(), f = (int)fsr4_why();
    } else if (v == ur::GAME) {
        if (variant < DLSS_WHY_COUNT) d = variant;
        else if (variant < DLSS_WHY_COUNT + FSR_WHY_COUNT - 1) f = variant - DLSS_WHY_COUNT + 1;
        else return nullptr;
    } else if (variant > 0) {
        return nullptr;
    }
    v = std::clamp(v, 0, ur::kUpscalers - 1);
    // (the reasons are said only where the engine is in the build: one without FSR does not tell an NVIDIA user why)
    const char* dn = dlss_built() || variant >= 0 ? dlss_no[d] : nullptr;
    const char* fn = fsr4_built() || variant >= 0 ? fsr_no[f] : nullptr;
    if (v != ur::GAME || (!dn && !fn)) return t[v];
    static char b[DLSS_WHY_COUNT][FSR_WHY_COUNT][320];
    snprintf(b[d][f], sizeof b[d][f], "%s%s%s%s%s", t[0], dn ? " " : "", dn ? dn : "", fn ? " " : "", fn ? fn : "");
    return b[d][f];
}
/// The DLSS model row (opts.dlss_model): NVIDIA's network by its preset, or RX_VK_DLSS_PRESET's for this run
const char* dlss_preset_env() {
    static const char* const e = getenv("RX_VK_DLSS_PRESET");
    return ur::preset_env(e) ? e : nullptr;
}
const char* tx_dlss_model(int v) {
    static const char* const t[] = {"Transformer (K)", "Transformer 2 (M)", "Transformer 2 (L)", "NVIDIA default"};
    if (const char* e = dlss_preset_env()) {
        static char b[48];
        const unsigned p = ur::dlss_preset(v, e);
        if (p) snprintf(b, sizeof b, "Preset %c (RX_VK_DLSS_PRESET)", ur::preset_letter(p));
        else snprintf(b, sizeof b, "NVIDIA default (RX_VK_DLSS_PRESET)");
        return b;
    }
    return t[std::clamp(v, 0, ur::kModels - 1)];
}
/// The DLSS model: the value chosen; variant 1: the game's own upscales (the row waits for DLSS or DLAA); variant 2: set for
/// this run by RX_VK_DLSS_PRESET
const char* hn_dlss_model(int v, int variant) {
    static const char* const t[] = {
        "NVIDIA's transformer model: the one tested most here.",
        "NVIDIA's second-generation transformer, its pick for Performance: sharper, at a little more GPU time.",
        "The second-generation transformer as NVIDIA tuned it for Ultra Performance, the smallest Render resolutions.",
        "Lets DLSS's runtime pick for each mode: what it picks can change with a runtime update.",
    };
    static const char* const unused = "Used by NVIDIA DLSS and DLAA: choose one in the Upscaler row.";
    static const char* const env = "Set for this run by RX_VK_DLSS_PRESET.";
    if (variant < 0) variant = dlss_preset_env() ? 2 : upscaler() == ur::GAME ? 1 : 0;
    if (variant == 1) return unused;
    if (variant == 2) return env;
    return variant == 0 ? t[std::clamp(v, 0, ur::kModels - 1)] : nullptr;
}
const char* tx_aniso(int v) {   // (Triforce: the Texture filtering row)
    static const char* t[] = {"Game's", "2x", "4x", "8x", "16x"};
    if ((unsigned)v >= sizeof t / sizeof t[0]) return t[0];
    return t[v];
}
const char* tx_percent(int v) {
    static char b[8];
    snprintf(b, sizeof b, "%d%%", v);
    return b;
}
// (local, RenoDX port: vk_renodx.cpp)
const char* tx_rdx_tonemap(int v) { return v ? "PsychoV35" : "Vanilla"; }
const char* tx_rdx_midgray(int v) { return v == 1 ? "Vanilla" : v == 2 ? "Eye adaptation" : "Render"; }
const char* tx_rdx_space(int v) { return v == 1 ? "BT.709" : v == 2 ? "DCI-P3" : "Vanilla"; }
const char* tx_rdx_grade(int v) {
    static char b[8];
    snprintf(b, sizeof b, "%d", v);
    return b;
}
// The output the menu's sizes are shown for: the Output resolution row's (applied at the next start), so the rows below follow a change
// at once; the handheld profile runs the game's own (1536x864)
struct Out {
    uint32_t w, h;
};
constexpr Out kOutputs[] = {{2048, 1152}, {2560, 1440}, {3840, 2160}, {1920, 1080}, {5120, 1440}, {5120, 2880}};
constexpr int kLastOutput = std::size(kOutputs) - 1;
int output_choice(int v) { return v >= 0 && v <= kLastOutput ? v : 0; }
// Display the sizes from smallest to largest while keeping the IDs used by saved settings.
constexpr int kOutputOrder[] = {3, 0, 1, 2, 4, 5};
static_assert(std::size(kOutputOrder) == std::size(kOutputs));
int step_output_choice(int id, int dir) {
    const int* pos = std::find(std::begin(kOutputOrder), std::end(kOutputOrder), output_choice(id));
    const int next = std::clamp((int)(pos - std::begin(kOutputOrder)) + dir, 0, kLastOutput);
    return kOutputOrder[next];
}
Out shown_output() {
    return opts.handheld ? Out{1536, 864} : kOutputs[output_choice(opts.output_res)];
}
// The size the game draws the 3D at for a fixed scale of its output, a twelfth of it: every twelfth asked came out exact at the three
// outputs (2048x1152: 0.58 drew 1195x672, 0.6667 1366x768). So the scale the game is given is the twelfth too: 0.67 drew 2615x1471 at
// 3840x2160, not the 2560x1440 of 8/12.
int twelfths(float scale) { return std::min(std::max((int)(scale * 12.0f + 0.5f), 1), 12); }
void game_size(float scale, uint32_t& w, uint32_t& h) {
    const Out o = shown_output();
    const int k = twelfths(scale);
    w = (o.w * k + 11) / 12, h = o.h * k / 12;
}
const char* tx_resolution(int v) {
    static char b[48];
    const int up = upscaler();
    if (up == ur::DLAA) return "Native (DLAA)";
    if (up == ur::FSR4_AA) return "Native (FSR 4)";
    if (!ur::percent(v)) return "Auto (dynamic)";
    if (const char* mode = ur::scales_by_mode(up) ? ur::dlss_mode_name(v) : nullptr) {
        snprintf(b, sizeof b, "%s (%d%%)", mode, ur::percent(v));
        return b;
    }
    uint32_t w, h;
    game_size(ur::percent(v) / 100.0f, w, h);
    snprintf(b, sizeof b, "%d%% (%ux%u)", ur::percent(v), w, h);
    return b;
}
/// The Render resolution: the value chosen, for the Upscaler shown (every one: each value with each Upscaler)
const char* hn_resolution(int v, int variant) {
    const int up = variant < 0 ? std::clamp(upscaler(), 0, ur::kUpscalers - 1) : variant;
    if (up >= ur::kUpscalers) return nullptr;
    if (up == ur::DLAA)
        return "DLAA draws the 3D at full size: nothing to set here. Choose DLSS or the game's own upscaler to draw it "
               "smaller.";
    if (up == ur::FSR4_AA)
        return "FSR 4's native AA draws the 3D at full size: nothing to set here. Choose FSR 4 or the game's own upscaler "
               "to draw it smaller.";
    const int p = ur::percent(v);
    if (up == ur::DLSS) {
        switch (p) {
        case 0: return "The game lowers it when the GPU is busy, and DLSS follows. Set Sharpness to around 80%.";
        case 67: return "DLSS's sharpest mode. Set Sharpness to around 80%.";
        case 58: return "Between Quality and Performance. Set Sharpness to around 80%.";
        case 50: return "Faster, and softer. Set Sharpness to around 80%.";
        case 33: return "DLSS's fastest mode, and its softest. Set Sharpness to around 80%.";
        default: return nullptr;   // (not one of DLSS's steps: the row moves to one)
        }
    }
    if (up == ur::FSR3 || up == ur::FSR4) {   // (the same modes, AMD's ratios)
        switch (p) {
        case 0: return "The game lowers it when the GPU is busy, and FSR follows.";
        case 67: return "FSR's Quality mode, 1.5x: its sharpest.";
        case 58: return "FSR's Balanced mode, 1.7x.";
        case 50: return "FSR's Performance mode, 2x: faster, and softer.";
        case 33: return "FSR's Ultra Performance mode, 3x: its fastest, and its softest.";
        default: return nullptr;   // (not one of FSR's steps: the row moves to one)
        }
    }
    if (!p) return "The game lowers it when the GPU is busy.";
    if (p == 100) return "The 3D at the output's full size: the sharpest, and the slowest.";
    return "A steady size: lower is faster, and softer.";
}
/// A row with nothing else to choose here: shown without its arrows, Left and Right leave it (and the saved value) alone
bool row_fixed(const int* value) {
    return (value == &opts.reflex && !reflex_available()) || (value == &opts.antilag && !antilag_available()) ||
           (value == &opts.dlss && ur::upscaler_fixed(upscalers_available())) ||
           (value == &opts.resolution && ur::resolution_fixed(upscaler())) || (value == &opts.upscale && !filter_needed()) ||
           (value == &opts.dlss_model && ur::model_fixed(upscaler(), dlss_preset_env() != nullptr));
}
const char* tx_output_res(int v) {
    static char b[32];
    v = output_choice(v);
    const Out o = kOutputs[v];
    snprintf(b, sizeof b, "%ux%u%s", o.w, o.h, v ? "" : " (game's)");
    return b;
}
const char* tx_speed(int v) { return v ? "Real time" : "Per frame"; }
const char* tx_text_speed(int v) {
    static const char* t[] = {"Normal", "2x", "4x", "Instant"};
    if ((unsigned)v >= sizeof t / sizeof t[0]) return t[0];
    return t[v];
}
const char* tx_ultrawide(int v) { return v ? "Monitor's shape" : "16:9"; }
const char* tx_prompts(int v) {
    static const char* t[HOS_PROMPT_N + 1] = {"Auto", "Switch", "Xbox", "PlayStation 4", "PlayStation 5", "Steam Deck"};
    return t[std::min(std::max(v, 0), (int)HOS_PROMPT_N)];
}
const char* tx_gpu(int v) {
    static char buf[300];
    const int idx = v - 1;
    if (v <= 0) snprintf(buf, sizeof buf, "Auto");
    else if (idx < (int)gpu_device_names.size()) snprintf(buf, sizeof buf, "%d: %s", idx, gpu_device_names[idx].c_str());
    else snprintf(buf, sizeof buf, "%d: Device %d", idx, idx);
    // (the whole name shows: the selected row slides through it, see marquee_pos)
    return buf;
}
const char* tx_pad_layout(int v);
// (hookshot) the Microphone row: Off, the system's default, or one of the host's microphones (host/nn_audio_in.c: the
// ocarina's input streams), told to the host by its name (host->mic_select) whenever it changes. The chosen one is kept by
// its name, mic_want, also while it is unplugged: the row then has one more entry for it, "<name> (not connected)", the
// host lets the default stand in and opens it again once it is back, and the name stays in the ini. In the ini the name is
// cut to kMicNameKept bytes (at a character's start), so the line fits the 80-byte readers of the file (host/nn_hid.c,
// nn_applet.c, hos_main.c), and a name is matched by that much of it
constexpr int kMaxMics = 16;
constexpr size_t kMicNameKept = 60;
std::string mic_want;      // (the chosen microphone's name, cut to kMicNameKept; empty: Off or the default chosen)
bool mic_missing = false;  // (mic_want is not plugged in: the row's last entry, 2 + mic_listed, stands for it)
int mic_listed = 0;        // (the microphones as the host last listed them)
bool can_mic() { return host && host->mic_count && host->mic_name && host->mic_select; }
size_t text_char_start(const std::string& text, size_t at) {
    while (at > 0 && at < text.size() && ((unsigned char)text[at] & 0xc0) == 0x80) at--;
    return at;
}
std::string mic_key(const char* name) {
    std::string k(name ? name : "");
    for (char& c : k)   // (a device's name goes into triforce_options.ini: a line break in it would add lines there)
        if ((unsigned char)c < 0x20 || c == 0x7f) c = ' ';
    if (k.size() <= kMicNameKept) return k;
    return k.substr(0, text_char_start(k, kMicNameKept));
}
/// buf cut back to whole UTF-8 characters (snprintf may have cut one)
void utf8_whole(char* buf) {
    size_t n = strlen(buf), i = n;
    while (i > 0 && ((unsigned char)buf[i - 1] & 0xC0) == 0x80) i--;   // (the last character's continuation bytes)
    if (i == 0) return;
    const unsigned char lead = (unsigned char)buf[i - 1];
    const size_t want = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if (n - (i - 1) < want) buf[i - 1] = 0;
}
const char* tx_mic(int v) {
    static char buf[160];
    if (v <= 0) return "Off";
    if (v == 1) return "Default";
    if (mic_missing && v == 2 + mic_listed) {
        const int room = (int)sizeof buf - (int)sizeof " (not connected)";
        snprintf(buf, (size_t)room + 1, "%s", mic_want.c_str());
        utf8_whole(buf);
        strcat(buf, " (not connected)");
        return buf;
    }
    const char* n = can_mic() ? host->mic_name(v - 2) : "";
    snprintf(buf, sizeof buf, "%s", n && *n ? n : "Microphone");
    utf8_whole(buf);
    return buf;
}
/// The Microphone row: Off, the default, a microphone of the list, or the chosen one while it is unplugged (variant 1)
const char* hn_mic(int v, int variant) {
    if (v <= 0) return variant <= 0 ? "The game hears silence." : nullptr;
    if (v == 1)
        return variant <= 0 ? "The system's default recording device. On Windows, desktop apps must be allowed to use the "
                              "microphone (Settings, Privacy & security)."
                            : nullptr;
    const bool missing = variant < 0 ? mic_missing && v == 2 + mic_listed : variant == 1;
    if (variant > 1) return nullptr;
    return missing ? "Not plugged in: Default stands in until it is back." : "This microphone, by its name. A change applies at once.";
}
const char* tx_rumble(int v) { return v ? tx_percent(v) : "Off"; }
// (hookshot) Menu size: how large the overlays are drawn (vk_ui.h unit); Auto is Large in fullscreen at 1080 lines and up while
// a controller is the menu's input, else Medium
bool pad_last = false;   // (a controller was the menu's last input: its buttons in the prompts)
// (Auto's choice: the controller or not, decided as the menu opens by the input that opened it. A controller's input makes it
// Large at any time; the keys and the mouse never make it smaller while the menu is open, so it never changes size under the
// cursor)
bool auto_pad = false;
int pad_confirm = 0, pad_back = 1;   // (the face buttons that confirm and go back, HOS_PAD_*: menu_keys::confirm_back, each tick)
bool auto_large(uint32_t h) { return opts.fullscreen && h >= 1080 && auto_pad; }
const char* tx_menu_size(int v) {
    static const char* t[] = {"Auto", "Small", "Medium", "Large"};
    if (v == 0) return auto_large(dev.swap_extent.height) ? "Auto (Large)" : "Auto (Medium)";
    return (unsigned)v < 4 ? t[v] : t[0];
}
const char* tx_stick_dead_zone(int v) {
    static const char* t[] = {"Default (24%)", "5%", "10%", "15%", "20%", "30%"};
    return (unsigned)v < 6 ? t[v] : t[0];
}
// (hookshot) Audio output: the speakers the game mixes for, read once at its start (host/nn_audio.c au_menu_output)
const char* tx_audio_output(int v) {
    static const char* t[] = {"Auto", "Stereo", "5.1"};
    return (unsigned)v < 3 ? t[v] : t[0];
}
// (hookshot) the motion's deadzone: the console's three zero-drift modes, the rate under which the pad counts as still
const char* tx_deadzone(int v) {
    static const char* t[] = {"Low", "Standard", "High"};
    return t[v < 0 ? 0 : v > 2 ? 2 : v];
}
const char* tx_robust(int v) {
    static const char* t[] = {"Auto", "Off", "On"};
    return (unsigned)v < 3 ? t[v] : t[0];
}
const char* tx_preset(int v) {
    static const char* t[] = {"Custom", "Minimum (4GB)", "Low (6GB)", "Balanced (8GB)", "High (12GB)", "Ultra (16GB+)"};
    return t[std::min(std::max(v, 0), 5)];
}
void sync_preset_budgets(int p) {
    switch (p) {
    case 1: opts.vram_mb = 2200;  opts.mirror_mb = 256;  break;
    case 2: opts.vram_mb = 3600;  opts.mirror_mb = 512;  break;
    case 3: opts.vram_mb = 4800;  opts.mirror_mb = 1024; break;
    case 4: opts.vram_mb = 7500;  opts.mirror_mb = 2048; break;
    case 5: opts.vram_mb = 11000; opts.mirror_mb = 3072; break;
    default: opts.vram_mb = 0;    opts.mirror_mb = 0;    break;
    }
}
// (Sharpness: tuned for the window pass's neighbours one picture pixel apart, each about the old setting's sharpening on a
// 3840 x 2160 window at that preset's picture, 2048 x 1152 or the handheld profile's 1536 x 864: same frame, mean |Laplacian|
// of the window and of the picture, on the slider's steps)
void apply_preset(int p) {
    switch (p) {
    case 1: // Minimum (4GB VRAM / iGPU)
        opts.resolution = 4; opts.upscale = 2; opts.sharpness = 30;
        opts.pace = 1;    opts.realtime = 1; opts.handheld = 1;
        break;
    case 2: // Low (6GB VRAM)
        opts.resolution = 2; opts.upscale = 2; opts.sharpness = 30;
        opts.pace = 1;    opts.realtime = 1; opts.handheld = 1;
        break;
    case 3: // Balanced (8GB VRAM)
        opts.resolution = 1; opts.upscale = 1; opts.sharpness = 30;
        opts.pace = 1;    opts.realtime = 1; opts.handheld = 0;
        break;
    case 4: // High (12GB VRAM)
        opts.resolution = 1; opts.upscale = 2; opts.sharpness = 40;
        opts.pace = 1;    opts.realtime = 1; opts.handheld = 0;
        break;
    case 5: // Ultra (16GB+ VRAM)
        opts.resolution = 1; opts.upscale = 2; opts.sharpness = 50;
        opts.pace = 1;    opts.realtime = 1; opts.handheld = 0;
        break;
    default:
        break;
    }
    // (the Upscaler and Frame generation are the player's: a preset never turns DLSS or made frames on or off. With DLSS
    // its resolution is DLSS's nearest mode)
    if (p > 0) opts.resolution = ur::snap(upscaler(), opts.resolution);
    sync_preset_budgets(p);
}

MENU_HINTS(hn_preset, 0, "Each row as you set it. Changing most rows yourself comes back to Custom.",
           "Resolution 50% (with DLSS its nearest mode), Lanczos, Sharpness 30%, Handheld profile.",
           "Resolution 75% (with DLSS its nearest mode), Lanczos, Sharpness 30%, Handheld profile.",
           "Resolution 100% (with DLSS its nearest mode), Catmull-Rom, Sharpness 30%.",
           "Resolution 100% (with DLSS its nearest mode), Lanczos, Sharpness 40%.",
           "Resolution 100% (with DLSS its nearest mode), Lanczos, Sharpness 50%.")
MENU_HINTS(hn_gpu, 0, "The best dedicated GPU.")
/// Frame generation: how many frames are made for each of the game's
const char* hn_framegen(int v, int variant) {
    // (2x: by AMD FSR 3.1 where it runs, from the scene's depth and motion: vk_framegen.cpp. Variant 1 is that text)
    if (v == 2 && (variant < 0 ? fsr3fi::frames_wanted() != 0 : variant == 1))
        return "One made frame between each two of the game's, by AMD FSR 3.1 from the scene's depth and motion.";
    if (variant > 0) return nullptr;
    if (v <= 0) return "Only the game's own frames: no added delay.";
    if (v == 1) return "A frame for each of the display's refreshes: none made when the game keeps up. Frame pacing does not apply.";
    static char b[9][96];
    snprintf(b[v], sizeof b[v], "%s between each two of the game's.", v == 2 ? "One made frame" : v == 3 ? "Two made frames" : v == 4 ? "Three made frames"
             : v == 5 ? "Four made frames" : v == 6 ? "Five made frames" : v == 7 ? "Six made frames" : "Seven made frames");
    return b[std::clamp(v, 2, 8)];
}
MENU_HINTS(hn_pace, 0, "As soon as it is ready.", "At an even interval: less stutter, at up to one frame of delay.")
MENU_HINTS(hn_output_res, 0, "The game's own size.",
           "The sharpest on a 2560x1440 screen: nothing to scale to the window.",
           "The sharpest on a 3840x2160 screen. The game's render targets get a bigger GPU pool, so a full-size 3D fits.",
           "The sharpest on a 1920x1080 screen: the HUD and text pixel for pixel, nothing to scale to the window.",
           "The sharpest on a 5120x1440 ultrawide screen: the whole frame pixel for pixel. The game's render targets get a bigger GPU pool, so a full-size 3D fits.",
           "The sharpest on a 5120x2880 (5K) screen: the whole frame pixel for pixel. The game's render targets get a bigger GPU pool, so a full-size 3D fits.")
/// The Window scaling filter: its filter, or that none is needed at this window's size
const char* hn_filter(int v, int variant) {
    static const char* const t[] = {"The plain copy.", "Sharper.", "Sharper still; can ring on edges.",
                                    "AMD FSR 1: rebuilds edges as it enlarges, with Sharpness as its own sharpening."};
    static const char* const none = "The window is the Output resolution's size: nothing to scale. The choice is kept for "
                                    "another size.";
    if (variant < 0) return filter_needed() ? t[std::clamp(v, 0, 3)] : none;
    return variant == 0 ? t[std::clamp(v, 0, 3)] : variant == 1 ? none : nullptr;
}
/// Sharpness: with DLSS or DLAA, the amount that suits them
const char* hn_sharpness(int v, int variant) {
    static const char* const dlss = "DLSS and DLAA look softer than the game's own upscaler: around 80% suits them.";
    const bool with_dlss = variant < 0 ? upscaler() == ur::DLSS || upscaler() == ur::DLAA : variant == 1;
    if (variant > 1) return nullptr;
    return with_dlss ? dlss : v == 0 ? "Off: the picture as it is." : nullptr;
}
MENU_HINTS(hn_handheld, 0, "Docked: the Output resolution's size.", "Lower resolutions, lighter on the GPU.")
MENU_HINTS(hn_ultrawide, 0, "The game's own shape.", "The camera takes the screen's shape, wider or narrower. The HUD keeps its 16:9 shape in the middle.")
const char* tx_wide_menus(int v) {
    static const char* t[] = {"Full width", "Centred, blurred sides", "Centred, black bars"};
    return t[std::min(std::max(v, 0), 2)];
}
MENU_HINTS(hn_wide_menus, 0, "The menus stretched over the whole screen, as before.",
           "The menus in the middle 16:9; beside them, what the game draws there, out of focus.",
           "The menus in the middle 16:9; black beside them.")
MENU_HINTS(hn_robust, 0, "On, on every card.", "Can be faster on some NVIDIA cards.", "Always on.")
MENU_HINTS(hn_fps, 0, nullptr, "With frame generation on, the game's own rate too.")
MENU_HINTS(hn_menu_size, 0, "Large in fullscreen on a screen of 1080 lines or more while a controller is in use, else Medium.",
           "Smaller, for a desk.", "The usual size.", "Larger, for a TV across the room.")
MENU_HINTS(hn_speed, 0, "A step each frame: the game slows down whenever it runs below 60 fps.",
           "Normal speed down to 15 fps, by taking longer steps.")
MENU_HINTS(hn_text_speed, 0, "The game's own pace, with its pauses.", "Twice as fast.", "Four times as fast.",
           "Each page at once, as pressing A does.")
MENU_HINTS(hn_audio_output, 0, "5.1 when the default output device has 5.1 or more speakers, else stereo.",
           "Stereo, whatever the device has.", "5.1, whatever the device has. Mixed down on stereo speakers.")
MENU_HINTS(hn_background, 0, "Only while the game's window has the focus.",
           "Also behind other windows, for streaming or other apps. The keyboard and mouse still need the game's window.")
MENU_HINTS(hn_mousecam, 0, "The mouse stands in for the right stick, with its curve and top speed.",
           "The mouse turns the camera itself, one to one, as in a PC game.")
MENU_HINTS(hn_mouse_invert, 0, nullptr, "Moving the mouse forward tilts the camera down.")
MENU_HINTS(hn_gyro, 0, "The sticks alone aim.",
           "Switch Pro controllers and pads in Switch mode, DualSense and DualShock 4. The value says whether the "
           "connected pad has a gyroscope.")
MENU_HINTS(hn_pad_layout, 0, "The Switch's order: A on the right, B at the bottom, X at the top, Y on the left.",
           "A at the bottom (action and talk), B on the right, X on the left, Y at the top.",
           "A at the bottom, B on the right; X and Y as on the Switch.",
           "Bindings of your own, from the Controls page.")
MENU_HINTS(hn_prompts, 0,
           "Those of the first controller, from its name (SDL's). The Switch's when it has none, as with XInput alone.")

Row rows[] = {
    // Display: the window and the screen
    {TAB_DISPLAY, "Fullscreen", &opts.fullscreen, 0, 1, 1, tx_onoff,
     "Fills the whole screen with the game, without the window's border. The same as F10 or Alt+Enter.", nullptr, false, true},
    // (the picture's way, top to bottom: the output's size, what fills it from the 3D, the 3D's size, then the window)
    {TAB_DISPLAY, "Output resolution", &opts.output_res, 0, kLastOutput, 1, tx_output_res,
     "The game's whole frame, HUD included, before it is scaled to the window. Docked only. Applies after a restart.",
     hn_output_res, true},
    {TAB_DISPLAY, "Ultrawide", &opts.ultrawide, 0, 1, 1, tx_ultrawide,
     "The camera's shape on a screen wider or narrower than 16:9 (4:3, 16:10). Applies after a restart.", hn_ultrawide, true},
    {TAB_DISPLAY, "Ultrawide menus", &opts.wide_menus, 0, 2, 1, tx_wide_menus,
     "With Ultrawide: the pause, item and map menus, and what shows beside them. Loading screens and pre-rendered movies, "
     "which draw no world either, get the same sides. Changes at once.", hn_wide_menus},
    {TAB_DISPLAY, "HDR", &opts.hdr, 0, 2, 1, tx_hdr,
     "HDR10 out on an HDR display (Windows: Use HDR on in the display settings). Expanded: the game's SDR picture, its white at "
     "the paper white and its highlights up to the peak. Native: the game's own HDR, told the display's peak and paper white.",
     nullptr, false, true},
    {TAB_DISPLAY, "HDR paper white", &opts.hdr_white, 80, 400, 10, tx_nits,
     "How bright the picture's white is with HDR on. Around 200 for a normal room; higher in a bright one.", nullptr, false, true},
    {TAB_DISPLAY, "HDR peak brightness", &opts.hdr_peak, 400, 4000, 100, tx_nits,
     "The brightest highlights with HDR on: set it to your display's peak brightness.", nullptr, false, true},
    // (Triforce, 11-13: RenoDX is in every build and off until this row turns it on. The scene's format is chosen at the
    // start, host/nn_vi.c hos_scene_rgba16f: a restart)
    {TAB_DISPLAY, "RenoDX HDR", &opts.renodx, 0, 1, 1, tx_onoff,
     "With HDR set to Native: RenoDX's tone mapping, eye adaptation and colour grading in place of the game's own, and its "
     "rows below. Off: the game's own HDR. Applies after a restart. RenoDX is by Carlos Lopez (clshortfuse), under the MIT "
     "license.", nullptr, true, true},
    // (local, RenoDX port: vk_renodx.cpp. Shown with HDR Native, where its shaders run)
    {TAB_DISPLAY, "RenoDX tonemapper", &opts.rdx_tonemap, 0, 1, 1, tx_rdx_tonemap,
     "With HDR Native: how the scene's light is brought to the display. Vanilla: the game's own curve. PsychoV35: RenoDX's, "
     "which keeps hue and saturation into the highlights, up to the peak brightness. RenoDX is by Carlos Lopez "
     "(clshortfuse), under the MIT license.", nullptr, false, true},
    {TAB_DISPLAY, "RenoDX midgray", &opts.rdx_midgray, 0, 2, 1, tx_rdx_midgray,
     "With PsychoV35: what the scene's middle grey is held to. Render: as rendered. Vanilla: where the game's own curve puts it. "
     "Eye adaptation: it follows what is on screen over a few seconds, weighted towards the middle of the view, as eyes adjust.",
     nullptr, false, true},
    {TAB_DISPLAY, "RenoDX UI brightness", &opts.rdx_ui_white, 80, 400, 10, tx_nits,
     "How bright the menus' and the HUD's white is with RenoDX's tonemapper. The scene's follows HDR paper white.", nullptr, false, true},
    {TAB_DISPLAY, "RenoDX color space", &opts.rdx_space, 0, 2, 1, tx_rdx_space,
     "The colours the picture is taken to be in before HDR10's. Vanilla: the game's own matrix. BT.709: as the SDR picture. "
     "DCI-P3: more saturated.", nullptr, false, true},
    {TAB_DISPLAY, "RenoDX exposure", &opts.rdx_exposure, 0, 200, 5, tx_percent,
     "The scene's brightness before RenoDX's tone mapping. 100% is the game's.", nullptr, false, true},
    {TAB_DISPLAY, "RenoDX highlights", &opts.rdx_highlights, 0, 100, 5, tx_rdx_grade,
     "RenoDX's colour grading: the bright parts of the scene. 50 is neutral.", nullptr, false, true},
    {TAB_DISPLAY, "RenoDX shadows", &opts.rdx_shadows, 0, 100, 5, tx_rdx_grade,
     "RenoDX's colour grading: the dark parts of the scene. 50 is neutral.", nullptr, false, true},
    {TAB_DISPLAY, "RenoDX contrast", &opts.rdx_contrast, 0, 100, 5, tx_rdx_grade,
     "RenoDX's colour grading: the scene's contrast. 50 is neutral.", nullptr, false, true},
    {TAB_DISPLAY, "RenoDX saturation", &opts.rdx_saturation, 0, 100, 5, tx_rdx_grade,
     "RenoDX's colour grading: the scene's saturation. 50 is neutral.", nullptr, false, true},
    {TAB_DISPLAY, "RenoDX bloom", &opts.rdx_bloom, 0, 100, 5, tx_rdx_grade,
     "How strong the glow around bright things is. 50 is the game's.", nullptr, false, true},
    {TAB_DISPLAY, "Game frame rate cap", &opts.fps_cap, 0, 5, 1, tx_fps_cap,
     "Limits frames rendered by the game. Generated frames are additional. Takes a restart. "
     "Past 60 is experimental: the game keeps real time, but some cutscenes and bosses can misbehave (Gohma, the intro's "
     "narration). 60 is the safe choice.",
     nullptr, true, true},
    {TAB_DISPLAY, "FPS counter", &opts.show_fps, 0, 1, 1, tx_onoff, "Frames per second in the top right corner.", hn_fps,
     false, true},
    {TAB_DISPLAY, "GPU device", &opts.gpu_device, 0, 8, 1, tx_gpu,
     "Which graphics card the game runs on. Applies after a restart.", hn_gpu, true, true},
    // Graphics: the picture, top to bottom (the Upscaler only in a build with DLSS, or where AMD FSR 3.1 runs: row_shown)
    {TAB_GRAPHICS, "Quality preset", &opts.preset, 0, 5, 1, tx_preset,
     "Sets the scaling rows and memory limits for a GPU with that much memory. The Upscaler and Frame generation stay yours.",
     hn_preset},
    {TAB_GRAPHICS, "Upscaler", &opts.dlss, 0, ur::kUpscalers - 1, 1, tx_upscaler, "What scales the 3D up to the Output resolution.",
     hn_upscaler},
    {TAB_GRAPHICS, "DLSS model", &opts.dlss_model, 0, ur::kModels - 1, 1, tx_dlss_model,
     "Which of NVIDIA's DLSS networks draws the picture.", hn_dlss_model, false, true},
    // (Triforce, 11-13: vk_temporal.cpp pre_wanted, off until asked)
    {TAB_GRAPHICS, "Upscale before bloom", &opts.upscale_pre, 0, 1, 1, tx_onoff,
     "With NVIDIA DLSS, DLAA or AMD FSR as the Upscaler: On, it works on the scene before the game's glow and tone mapping, "
     "so both are made from the sharpened scene. Off: on the finished picture, as before. Applies after a restart.",
     nullptr, true, true},
    // (Triforce: read once, at the start (host/nn_vi.c): Auto -> 100% in game left the 3D at the dynamic path's size)
    {TAB_GRAPHICS, "Render resolution", &opts.resolution, 0, ur::kValues - 1, 1, tx_resolution,
     "The share of the output the 3D is drawn at; the Upscaler fills in the rest. Applies after a restart.", hn_resolution, true},
    {TAB_GRAPHICS, "Window scaling filter", &opts.upscale, 0, 3, 1, tx_filter,   // (Triforce: 3 = AMD FSR 1, vk_framegen.cpp)
     "How the finished picture is scaled to the window, when its size differs from the Output resolution.", hn_filter},
    {TAB_GRAPHICS, "Sharpness", &opts.sharpness, 0, 100, 10, tx_percent,
     "Sharpens the picture as it goes to the window, with any filter. Too much shows halos around edges.", hn_sharpness},
    {TAB_GRAPHICS, "Texture filtering", &opts.aniso, 0, 4, 1, tx_aniso,   // (Triforce: vk_draw.cpp sampler_for)
     "Anisotropic filtering: keeps ground, paths and walls sharp where they slope away from the camera. Game's is what the game "
     "asks for; 16x is the sharpest, at a small cost."},
    {TAB_GRAPHICS, "Frame generation", &opts.framegen, 0, 8, 1, tx_framegen,
     "Makes frames between the game's own, for smoother motion, at a little input delay. Fast motion or camera cuts can "
     "show artifacts.", hn_framegen, false, true},
    {TAB_GRAPHICS, "Frame pacing", &opts.pace, 0, 1, 1, tx_onoff, "When each frame is shown.", hn_pace},
    {TAB_GRAPHICS, "NVIDIA Reflex", &opts.reflex, 0, 2, 1, tx_reflex,
     "NVIDIA GPUs: each frame starts as late as it can, for less input delay. Not used with frame generation.", hn_reflex},
    {TAB_GRAPHICS, "AMD Anti-Lag 2", &opts.antilag, 0, 1, 1, tx_antilag,
     "AMD GPUs: each frame starts as late as it can, for less input delay. Not used with frame generation.", hn_antilag},
    {TAB_GRAPHICS, "Handheld profile", &opts.handheld, 0, 1, 1, tx_onoff,
     "Runs the game as if the console were undocked. Applies after a restart.", hn_handheld, true},
    {TAB_GRAPHICS, "Robust buffers", &opts.robust, 0, 2, 1, tx_robust,
     "Bounds-checked buffer reads: a read past a buffer's end is dropped instead of stopping the GPU. Applies after a "
     "restart.", hn_robust, true},

    {TAB_SYSTEM, "Game speed", &opts.realtime, 0, 1, 1, tx_speed, "How the game keeps time when it runs below 60 fps.",
     hn_speed},
    {TAB_SYSTEM, "Text speed", &opts.text_speed, 0, 3, 1, tx_text_speed,
     "How fast the letters of dialogue and other messages appear.", hn_text_speed},
    {TAB_AUDIO, "Audio output", &opts.audio_output, 0, 2, 1, tx_audio_output,
     "The speakers the game mixes its sound for. Applies after a restart.", hn_audio_output, true, true},
    {TAB_AUDIO, "Microphone", &opts.mic, 0, 2 + kMaxMics, 1, tx_mic,
     "What the ocarina hears when the game's microphone option is on (playing an instrument, or humming).", hn_mic, false,
     true},
    {TAB_SYSTEM, "Discord status", &opts.discord, 0, 1, 1, tx_onoff,
     "Shows the game and the time played in your Discord status, while Discord runs on this PC. Triforce has no Discord "
     "application of its own: it needs discord_app_id= in triforce_options.ini.", nullptr, false,
     true},

    // Controls (11 rows; Motion opens the 6 TAB_MOTION rows, Controls its page with the 2 TAB_PAGE rows)
    {TAB_CONTROLS, "Controller in background", &opts.pad_background, 0, 1, 1, tx_onoff,
     "Whether the controller plays the game while another window has the focus.", hn_background},
    {TAB_CONTROLS, "Mouse camera", &opts.mouse_native, 0, 1, 1, tx_mousecam, "How the captured mouse turns the camera.",
     hn_mousecam, false, true},
    {TAB_CONTROLS, "Mouse sensitivity", &opts.mouse_sens, 10, 500, 10, tx_sens,
     "How far the camera turns for a mouse movement, with either Mouse camera: at 1.00 native look turns 0.1 degree a "
     "count. The - and = keys change it in game too.", nullptr, false, true},
    {TAB_CONTROLS, "Invert mouse vertical", &opts.mouse_invert, 0, 1, 1, tx_onoff, "Swaps up and down for the mouse.",
     hn_mouse_invert, false, true},
    {TAB_CONTROLS, "Invert camera X", &opts.invert_cam_x, 0, 1, 1, tx_onoff,
     "Inverts horizontal look (left/right) on the right analog stick.", nullptr, false, true},
    {TAB_CONTROLS, "Invert camera Y", &opts.invert_cam_y, 0, 1, 1, tx_onoff,
     "Inverts vertical look (up/down) on the right analog stick.", nullptr, false, true},
    {TAB_CONTROLS, "Stick dead zone", &opts.stick_dead_zone, 0, 5, 1, tx_stick_dead_zone,
     "How far a stick moves before the game sees it, on each axis. Lower for a precise stick, higher for one that drifts. "
     "The game's own dead zone still follows.", nullptr, false, true},
    {TAB_CONTROLS, "Rumble strength", &opts.rumble, 0, 150, 25, tx_rumble,
     "How strongly the controller rumbles, in percent of the game's rumble. Off turns it off.", nullptr, false, true},
    {TAB_MOTION, "Motion controls", &opts.gyro, 0, 1, 1, tx_gyro,
     "Aims and looks with the controller's gyroscope where the game uses motion, as on the Switch. Works with Switch Pro "
     "controllers and pads in Switch mode, DualSense and DualShock 4. Shown beside it: whether the connected pad has one.", hn_gyro,
     false, true},
    {TAB_MOTION, "Motion sensitivity X", &opts.gyro_sens_x, 10, 600, 10, tx_percent,
     "How far the aim moves left and right for a given turn of the controller. 100% is the console's own.", nullptr, false,
     true},
    {TAB_MOTION, "Motion sensitivity Y", &opts.gyro_sens_y, 10, 300, 10, tx_percent,
     "How far the aim moves up and down for a given tilt of the controller. 100% is the console's own.", nullptr, false,
     true},
    {TAB_MOTION, "Invert motion X", &opts.gyro_inv_x, 0, 1, 1, tx_onoff, "Turns the motion aim's left and right around.",
     nullptr, false, true},
    {TAB_MOTION, "Invert motion Y", &opts.gyro_inv_y, 0, 1, 1, tx_onoff, "Turns the motion aim's up and down around.",
     nullptr, false, true},
    {TAB_MOTION, "Motion deadzone", &opts.gyro_deadzone, 0, 2, 1, tx_deadzone,
     "How slow a turn counts as none, the console's three settings: Standard is its own. High steadies a pad that jitters "
     "when held still; Low keeps the smallest movements.", nullptr, false, true},
    {TAB_CONTROLS, "Motion", nullptr, 0, 0, 0, nullptr,   // (hookshot: opens the Motion page, the TAB_MOTION rows)
     "Motion controls: aiming with the controller's gyroscope, its sensitivity and its inverts."},
    // (hookshot) the Controls page's own rows, above its grid (TAB_PAGE: no tab shows them)
    {TAB_PAGE, "Layout preset", &opts.pad_layout, 0, 3, 1, tx_pad_layout,
     "Which face button is which. Binding one yourself on the Controls page makes it Custom.", hn_pad_layout},
    {TAB_PAGE, "Button prompts", &opts.prompts, 0, HOS_PROMPT_N, 1, tx_prompts,
     "The button icons the game shows, in its texts and menus: those of this controller, for the buttons your controls "
     "put each action on.", hn_prompts, true, true},
    {TAB_CONTROLS, "Controls", nullptr, 0, 0, 0, nullptr,   // (hookshot: opens its page)
     "The layout preset, the button prompts, and which gamepad button and which keys press each of the Switch's "
     "buttons."},
    {TAB_CONTROLS, "Menu shortcut", &opts.menu_shortcut, 0, 1, 1, tx_menu_shortcut,
     "Which keys open and close these options from anywhere. F1 always does; F1 only leaves Insert free for an overlay.",
     nullptr, false, true},
    // Debug: the tools' switch, the tools themselves below it while they are on (cheat: menu_tick does what they do), then the
    // developer's rows
    {TAB_DEBUG, "Debug tools", &opts.debug_tools, 0, 1, 1, tx_onoff,
     "Testing tools: no-clip flight, a free camera, teleport, Link's age, the inventory, the clock and heart protection. Off "
     "puts flight, the free camera and heart protection away.", nullptr, false, true},
    {TAB_DEBUG, "Heart protection", nullptr, 0, 0, 0, nullptr,
     "Protects Link's hearts. Hits can still knock him back.", nullptr, false, true, 0},
    {TAB_DEBUG, "No-clip flight", nullptr, 0, 0, 0, nullptr,
     "Enable double-tap Jump for takeoff and landing. Hold Jump to climb or L to descend.", nullptr, false, true, 1},
    {TAB_DEBUG, "Mark position", nullptr, 0, 0, 0, nullptr,
     "Remember Link's position for this loaded scene.", nullptr, false, true, 2},
    {TAB_DEBUG, "Return to mark", nullptr, 0, 0, 0, nullptr,
     "Teleport Link to the marked position in the current scene.", nullptr, false, true, 3},
    {TAB_DEBUG, "Set time", nullptr, 0, 0, 0, nullptr,
     "Choose an hour with Left or Right. Enter sets the world clock.", nullptr, false, true, 4},
    {TAB_DEBUG, "Inventory item", nullptr, 0, 0, 0, nullptr,
     "Choose a consumable with Left or Right.", nullptr, false, true, 5},
    {TAB_DEBUG, "Change quantity", nullptr, 0, 0, 0, nullptr,
     "Choose how many to add or remove. Enter applies the change.", nullptr, false, true, 6},
    {TAB_DEBUG, "Free camera", nullptr, 0, 0, 0, nullptr,
     "Move and look with your usual controls. Page Up / Page Down or R / L changes height.", nullptr, false, true, 7},
    {TAB_DEBUG, "Teleport", nullptr, 0, 0, 0, nullptr,
     "Opens the map list. Link travels through the game's own map change, in his current age.", nullptr, false, true, 8},
    {TAB_DEBUG, "Switch age", nullptr, 0, 0, 0, nullptr,
     "Swaps Link between child and adult and reloads this map in the other age (the Market when this map has no match).", nullptr, false, true, 9},
    {TAB_DEBUG, "Inventory", nullptr, 0, 0, 0, nullptr,
     "Enter opens the inventory editor: unlock or relock equipment, items, upgrades and quest items, and change amounts.", nullptr, false, true, 10},
    {TAB_DEBUG, "Developer", nullptr, 0, 0, 0, nullptr, nullptr, nullptr, false, true, -1, true},
    {TAB_DEBUG, "Descriptor reuse", &opts.descriptor_reuse, 0, 1, 1, tx_onoff,
     "A draw keeps its descriptors while the texture slots it samples are unchanged, so streaming elsewhere in the game no longer "
     "rebuilds every set each frame. Applies after restart.", nullptr, true},
    {TAB_DEBUG, "Sampler cache", &opts.sampler_cache, 0, 1, 1, tx_onoff,
     "What a sampler holds is kept beside it on the replay thread, so resolving a sampler that was not registered anew takes no "
     "lock. Helps while the game streams textures. Applies after restart.", nullptr, true},
    // (Restart and Quit stay the table's last two: kRestartRow and kQuitRow count back from its end)
    {TAB_SYSTEM, "Restart", nullptr, 0, 0, 0, nullptr,
     "Starts the game again. Save your progress first. Press Enter or A twice to restart. Esc or B "
     "cancels."},
    {TAB_SYSTEM, "Quit", nullptr, 0, 0, 0, nullptr,
     "Save your progress first. Press Enter or A twice to quit. Esc or B cancels."},
};
constexpr int kQuitRow = sizeof rows / sizeof rows[0] - 1;
constexpr int kRestartRow = kQuitRow - 1;
const Row* pending = nullptr;   // (Restart or Quit: shows Confirm until its second press)
bool cheats_env = false;   // (HOS_OOT_CHEATS=1: on whatever the Debug tab says)
/// The debug tools: the Debug tab's row, or HOS_OOT_CHEATS=1
bool cheats_on() { return cheats_env || opts.debug_tools; }
// (Triforce) The Debug tab is not shown at all until Alt+G unlocks it, and Alt+G again puts it away (and its tools off). Kept in
// triforce_options.ini as debug_tab=1, so it stays unlocked for the next start; HOS_OOT_CHEATS=1 shows it too
int debug_unlocked = 0;
bool debug_shown() { return debug_unlocked || cheats_env; }
int tabs_shown() { return debug_shown() ? TAB_COUNT : TAB_COUNT - 1; }
// (Triforce) Inside the game's own Options screen. host/oot_menu.c says each frame that screen runs (menu_option_set,
// HOS_OPT_GAME_OPTIONS). While it does, the options' tabs are drawn over its header with one more in front, "Game": the
// game's own list, which stays the game's (drawn by it, its input its own). L and R (the buttons the Switch's L and R are
// bound to, or their keys: Q and E are the game's own keys there) go on to our tabs and back; on ours the game's list,
// caption and guides are under the same plates, ours, which are opaque. Back on one of ours is the game's Back too
// (host/nn_hid.c). TRIFORCE_OPTIONS_TABS=0: off (the game's Options as it is, ours through F1 alone)
std::atomic<int64_t> game_options_ns{0};   // (prof_now at the last frame the host said so; 0 never)
bool embed_open = false;                   // (our tabs are open from inside the game's Options: leaving them is its own tab again)
int64_t embed_since = 0;                   // (when that screen was first seen running: the tabs fade in over its opening)
constexpr int kGameTab = 99;               // (the tabs' hit for the game's own tab)
bool embed_allowed() {
    static const bool on = !getenv("TRIFORCE_OPTIONS_TABS") || atoi(getenv("TRIFORCE_OPTIONS_TABS")) != 0;
    return on;
}
/// (Triforce, 11-13, the user: "Remove f1 menu (we have it in options now natively)") Whether Insert, F1, a gamepad's Home and
/// Select+Start open these options over the game from anywhere. Not any more: they are the tabs inside the game's own
/// Options screen. TRIFORCE_INGAME_MENU=1 (triforce_env.txt) brings the keys back; with TRIFORCE_OPTIONS_TABS=0 (no tabs in
/// the game's screen) they stay, or there would be no way to the options at all
bool overlay_allowed() {
    static const bool on = (getenv("TRIFORCE_INGAME_MENU") && atoi(getenv("TRIFORCE_INGAME_MENU")) != 0) || !embed_allowed();
    return on;
}
/// The game's Options screen ran within the last quarter second
bool game_options_shown() {
    const int64_t t = game_options_ns.load(std::memory_order_relaxed);
    return embed_allowed() && t && prof_now() - t < 250000000LL;
}
// (a line shown for a moment at the foot of the screen: the Debug tab unlocked or locked while the options are closed)
char notice_text[64] = "";
int64_t notice_until = 0;
std::atomic<int> protect_hearts{-1};
// -1 disables the shortcut, 0 enables it on the ground, and 1 means flying.
std::atomic<int> player_flight{-1};
std::atomic<int> free_camera{0};
std::atomic<int> cheat_action{0};
std::atomic<int> link_age{-1};   // (the game side's report, OOT_CHEAT_AGE_ID: 0 child, 1 adult, -1 no save loaded)
int clock_hour = 12;
int pouch_item = 0;
int pouch_delta = 1;
constexpr const char* pouch_items[] = {"Deku Seeds", "Deku Nuts", "Deku Sticks", "Bombs", "Arrows"};
constexpr int kPouchItems = sizeof pouch_items / sizeof pouch_items[0];
// (teleport list: generated by work/oot-teleport-tools/write_destinations.py)
// (the order of oot_destinations in host/hos_main.c; Enter sends 1000 + the index)
constexpr const char* teleport_names[] = {
    "Kokiri Forest",
    "Hyrule Field",
    "Kakariko Village",
    "Market",
    "Hyrule Castle",
    "Lost Woods",
    "Sacred Forest Meadow",
    "Lake Hylia",
    "Graveyard",
    "Death Mountain Trail",
    "Death Mountain Crater",
    "Goron City",
    "Zora's River",
    "Zora's Domain",
    "Zora's Fountain",
    "Gerudo Valley",
    "Gerudo Fortress",
    "Haunted Wasteland",
    "Desert Colossus",
    "Inside the Deku Tree",
    "Dodongo's Cavern",
    "Jabu-Jabu's Belly",
    "Forest Temple",
    "Fire Temple",
    "Water Temple",
    "Shadow Temple",
    "Spirit Temple",
    "Bottom of the Well",
    "Ice Cavern",
    "Gerudo Training Ground",
    "Ganon's Castle"};
constexpr const char* teleport_notes[] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "adult only", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
struct TeleportSection { const char* name; int first; };
constexpr TeleportSection teleport_sections[] = {{"Overworld", 0}, {"Dungeons", 19}};
constexpr int kTeleportSections = sizeof teleport_sections / sizeof teleport_sections[0];
// (end of the generated teleport list)
constexpr int kTeleports = sizeof teleport_names / sizeof teleport_names[0];
int teleport_choice = 0;
int teleport_columns = 1, teleport_page_rows = 1;   // (as last drawn: the cards in a row, the rows that fit: Page Up / Down)
float teleport_scroll_px = 0, teleport_want_px = 0;   // (the cards' scroll, eased toward teleport_want_px)
bool teleport_follow = true;   // (the scroll follows the choice; the wheel lets go of it until the choice moves)
int teleport_wheel = 0;        // (the wheel's notches since the last drawing: it scrolls by them)
int teleport_section_of(int i) {
    int section = 0;
    while (section + 1 < kTeleportSections && i >= teleport_sections[section + 1].first) ++section;
    return section;
}
int teleport_section_end(int section) {
    return section + 1 < kTeleportSections ? teleport_sections[section + 1].first : kTeleports;
}
/// The destination rows rows down (up when negative) from i, in the grid the Teleport page draws: each section's cards
/// teleport_columns to a row, under the section's heading; the column kept where the next row has it
int teleport_step_rows(int i, int rows) {
    const int cols = std::max(1, teleport_columns);
    for (; rows > 0; --rows) {
        const int section = teleport_section_of(i), first = teleport_sections[section].first, end = teleport_section_end(section);
        const int p = i - first;
        if (p + cols < end - first) i += cols;
        else if (p / cols < (end - first - 1) / cols) i = end - 1;   // (the section's last row, a short one)
        else if (section + 1 < kTeleportSections) i = std::min(end + p % cols, teleport_section_end(section + 1) - 1);
        else break;
    }
    for (; rows < 0; ++rows) {
        const int section = teleport_section_of(i), first = teleport_sections[section].first, p = i - first;
        if (p >= cols) i -= cols;
        else if (section > 0) {
            const int prev = teleport_sections[section - 1].first, n = first - prev;
            i = std::min(prev + (n - 1) / cols * cols + p, first - 1);
        } else break;
    }
    return i;
}
// the Inventory page (gpu/vk/oot_cheat_items.h): items and their section headings, counts from the game side
#define OOT_MENU_ITEM_LABEL(label, name, kind) label,
#define OOT_MENU_ITEM_KIND(label, name, kind) kind,
#define OOT_MENU_NO_SECTION(name)
constexpr const char* item_names[] = {OOT_CHEAT_ITEMS(OOT_MENU_NO_SECTION, OOT_MENU_ITEM_LABEL)};
constexpr unsigned char item_kinds[] = {OOT_CHEAT_ITEMS(OOT_MENU_NO_SECTION, OOT_MENU_ITEM_KIND)};
constexpr int kItems = sizeof item_names / sizeof item_names[0];
// (each section's first item: a counter that a section heading reads and an item bumps)
constexpr int item_section_count() {
    int n = 0;
#define OOT_MENU_COUNT_SECTION(title) n++;
#define OOT_MENU_SKIP_ITEM(label, name, kind)
    OOT_CHEAT_ITEMS(OOT_MENU_COUNT_SECTION, OOT_MENU_SKIP_ITEM)
    return n;
}
constexpr int kItemSections = item_section_count();
struct ItemSections { TeleportSection s[kItemSections]; };
constexpr ItemSections make_item_sections() {
    ItemSections r{};
    int section = 0, item = 0;
#define OOT_MENU_ADD_SECTION(title) r.s[section].name = title, r.s[section].first = item, section++;
#define OOT_MENU_ADD_ITEM(label, name, kind) item++;
    OOT_CHEAT_ITEMS(OOT_MENU_ADD_SECTION, OOT_MENU_ADD_ITEM)
    return r;
}
constexpr ItemSections item_sections = make_item_sections();
std::atomic<int> item_counts[kItems];   // (-1 until the game side reports: OOT_CHEAT_ITEM_BASE + i)
int item_choice = 0;
int inventory_columns = 1, inventory_capacity = 1;   // (as last drawn: the cards in a row, on a page)
int inventory_filter = 0;   // (All, then the item sections)
int inventory_page = 0;     // (the page shown: the choice's, or another the wheel turned to)
bool inventory_amount = false;   // (Left and Right change the chosen item's amount)
int inventory_begin() { return inventory_filter ? item_sections.s[inventory_filter - 1].first : 0; }
int inventory_end() { return inventory_filter && inventory_filter < kItemSections ? item_sections.s[inventory_filter].first : kItems; }
void inventory_reveal() { inventory_page = (item_choice - inventory_begin()) / std::max(1, inventory_capacity); }
void choose_inventory_filter(int filter) {
    inventory_filter = filter;
    item_choice = inventory_begin();
    inventory_amount = false;
    inventory_reveal();
}
constexpr int kCheatRows = 11;   // (the debug tools' rows: Row::cheat 0 to 10)
/// A debug tool's value as its row shows it ("" an action: confirm does it)
std::string cheat_value(int i) {
    char t[16];
    switch (i) {
    case 0: return tx_onoff(protect_hearts.load() == 1);
    case 1: return tx_onoff(player_flight.load() >= 0);
    case 4: return snprintf(t, sizeof t, "%02d:00", clock_hour), t;
    case 5: return pouch_items[pouch_item];
    case 6: return snprintf(t, sizeof t, "%+d", pouch_delta), t;
    case 7: return tx_onoff(free_camera.load());
    default: return "";
    }
}
/// The tools that choose with Left and Right (arrows at their value's ends): Set time, Inventory item, Change quantity
bool cheat_chooses(int i) { return i == 4 || i == 5 || i == 6; }
/// Switch age as Kid | Adult, Link's age lit: once the game side has said it (a save loaded)
bool age_shown(int i) { return i == 9 && link_age.load(std::memory_order_relaxed) >= 0; }
/// What confirm does on a tool's row, for its prompt (nullptr: nothing)
const char* cheat_confirm(int i) {
    static const char* const says[kCheatRows] = {"Toggle", "Toggle", "Apply", "Apply", "Set", nullptr, "Apply", "Toggle", "Open list",
                                                 "Switch", "Open"};
    return i >= 0 && i < kCheatRows ? says[i] : nullptr;
}
constexpr int kRows = sizeof rows / sizeof rows[0];

int cur_tab = TAB_DISPLAY;
int tab_selected[TAB_COUNT + 1] = {};   // (and the Motion page's)
bool is_motion_row(const Row& r) { return r.tab == TAB_CONTROLS && !r.value && !strcmp(r.name, "Motion"); }

// ---- the rows that take a restart: their values at the start (menu_init, the GPU's at menu_set_gpu_count) ----
namespace mr = menu_restart;
mr::State restart_state;
int restart_slot[kRows];             // (a row's slot in restart_state, -1 none)
const char* restart_names[mr::kMax];
void restart_init() {
    static bool done = false;
    if (done) return;
    done = true;
    for (int i = 0; i < kRows; i++) {
        restart_slot[i] = rows[i].restart && rows[i].value ? restart_state.add(*rows[i].value) : -1;
        if (restart_slot[i] >= 0) restart_names[restart_slot[i]] = rows[i].name;
    }
}
int restart_slot_of(const Row& r) { return restart_slot[&r - rows]; }
/// A row that takes a restart, changed from what the game started with (a row past menu_restart.h's kMax has no slot: never)
bool restart_changed(const Row& r) {
    const int slot = restart_slot_of(r);
    return r.value && r.restart && slot >= 0 && *r.value != restart_state.start[slot];
}
void restart_values(int* now) {
    for (int i = 0; i < kRows; i++)
        if (restart_slot[i] >= 0) now[restart_slot[i]] = *rows[i].value;
}
/// The rows whose value is not the one the game runs with (a bit a slot)
unsigned restart_pending() {
    int now[mr::kMax];
    restart_values(now);
    return restart_state.pending(now);
}
/// Whether closing the menu now asks to restart: something pending, not declined with Later, and a window to close
bool restart_ask() {
    int now[mr::kMax];
    restart_values(now);
    return app_window() && restart_state.ask(now);
}
std::string restart_list(const char* last) {
    return mr::names(restart_names, restart_state.n, restart_pending(), ", ", last);
}
int ask_sel = 0;   // (the restart question's choice: 0 Later, 1 Restart now)

/// A row on its tab: the debug tools only with them on, the Upscaler only in a build with DLSS or FSR 4, or where AMD
/// FSR 3.1 runs
bool rdx_row(const int* v) {   // (local, RenoDX port)
    return v == &opts.rdx_tonemap || v == &opts.rdx_midgray || v == &opts.rdx_ui_white || v == &opts.rdx_space || v == &opts.rdx_exposure ||
           v == &opts.rdx_highlights || v == &opts.rdx_shadows || v == &opts.rdx_contrast || v == &opts.rdx_saturation ||
           v == &opts.rdx_bloom;
}
bool row_shown(int i) {
    return (rows[i].cheat < 0 || cheats_on()) &&
           (rows[i].value != &opts.dlss || ur::upscaler_shown(dlss_built() || fsr4_built(), fsr3::supported())) &&
           (rows[i].value != &opts.dlss_model || ur::model_shown(dlss_built())) &&
           // (local, RenoDX port: its rows with HDR Native, in a build with its shaders; Triforce: and its own row on)
           (!rdx_row(rows[i].value) || (opts.hdr == 2 && renodx_built() && opts.renodx)) &&
           (rows[i].value != &opts.renodx || renodx_built()) &&
           // (Triforce, 11-13: the Menu shortcut row chooses the keys that open the options from anywhere: gone with them)
           (rows[i].value != &opts.menu_shortcut || overlay_allowed());
}
int tab_row_count(int t) {
    int c = 0;
    for (int i = 0; i < kRows; i++) if (rows[i].tab == (MenuTab)t && row_shown(i)) c++;
    return c;
}

int tab_row_to_global(int t, int local_idx) {
    int count = 0;
    for (int i = 0; i < kRows; i++) {
        if (rows[i].tab == (MenuTab)t && row_shown(i)) {
            if (count == local_idx) return i;
            count++;
        }
    }
    return 0;
}

std::atomic<bool> open_now{false};
bool menu_refresh = false;
// (a value too long for its row, a GPU's name: the selected row's slides through it, a character a step, resting at
// each end; the other rows end it with "...". The drawing (fit) says how far it can slide, menu_tick moves it)
int marquee_pos = 0, marquee_max = 0;
int64_t marquee_next = 0;
constexpr int64_t kMarqueeStep = 150000000LL, kMarqueeRest = 1200000000LL;   // (ns)
// (hookshot) the mouse (menu_tick): the tab strip's part under the cursor (a tab 0 to TAB_COUNT - 1, TAB_COUNT the previous,
// TAB_COUNT + 1 the next; -1 none); the slider held with the left button (its row in the tab, -1 none)
int hover_tab = -1;
int drag_row = -1;
bool mouse_read = false;   // (the menu has read the mouse since it opened)
bool is_slider(const Row& r) { return r.value == &opts.sharpness || r.value == &opts.mouse_sens || r.value == &opts.gyro_sens_x || r.value == &opts.gyro_sens_y; }
/// (a scale: the arrows and a click stop at its ends; Rumble strength would go from Off to its strongest)
bool stops_at_ends(const Row& r) { return r.value == &opts.mouse_sens || r.value == &opts.rumble || r.value == &opts.gyro_sens_x || r.value == &opts.gyro_sens_y; }
/// (the ini's Rumble strength on the menu's steps: 0 to 150, the nearest 25)
int rumble_step(int v) { return (std::min(std::max(v, 0), 150) + 12) / 25 * 25; }
std::string ini_path() {
    char exe[MAX_PATH];
    const DWORD n = game_module_path(exe, MAX_PATH);
    std::string dir(exe, n);
    return dir.substr(0, dir.find_last_of("\\/") + 1) + "triforce_options.ini";
}
const char* ini_key(const Row& r) {
    if (r.value == &opts.output_res) return "output_res";   // (host/nn_applet.c reads it at the start)
    // (rows renamed since their values were first saved keep their keys: an options file from before still loads)
    if (r.value == &opts.resolution) return "resolution";
    if (r.value == &opts.fps_cap) return "frame_rate_cap";   // (host/nn_vi.c reads it at the start)
    if (r.value == &opts.upscale) return "upscaling";
    if (r.value == &opts.dlss) return "dlss";   // (as opts.dlss and RX_VK_DLSS)
    static char k[32];
    size_t n = 0;
    for (const char* p = r.name; *p && n + 1 < sizeof k; p++) k[n++] = *p == ' ' ? '_' : (char)tolower(*p);
    k[n] = 0;
    return k;
}
// (hookshot) the controls, kept by the host (host/nn_hid.c reads them back from here): pad_<button>, key_<button>
const char* const bind_names[HOS_KEY_N] = {"a", "b", "x", "y", "ls", "rs", "l", "r", "zl", "zr", "plus", "minus",
                                           "left", "up", "right", "down", "move_up", "move_down", "move_left",
                                           "move_right", "cam_up", "cam_down", "cam_left", "cam_right"};
bool can_bind() { return host && host->binding && host->bind && host->pad_raw; }
const char* tx_pad_layout(int v) {
    static const char* t[] = {"Nintendo (Def)", "Xbox / PC", "Swap Face", "Custom"};
    return t[std::min(std::max(v, 0), 3)];
}
void apply_pad_layout(int layout) {
    if (!can_bind()) return;
    if (layout == 0) { // Nintendo (Default: Bottom=B, Right=A, Top=X, Left=Y)
        host->bind(HOS_BIND_PAD, HOS_SW_A, 0, HOS_PAD_EAST);
        host->bind(HOS_BIND_PAD, HOS_SW_B, 0, HOS_PAD_SOUTH);
        host->bind(HOS_BIND_PAD, HOS_SW_X, 0, HOS_PAD_NORTH);
        host->bind(HOS_BIND_PAD, HOS_SW_Y, 0, HOS_PAD_WEST);
    } else if (layout == 1) { // Xbox / PC Standard (Bottom=A, Right=B, Left=X, Top=Y)
        host->bind(HOS_BIND_PAD, HOS_SW_A, 0, HOS_PAD_SOUTH);
        host->bind(HOS_BIND_PAD, HOS_SW_B, 0, HOS_PAD_EAST);
        host->bind(HOS_BIND_PAD, HOS_SW_X, 0, HOS_PAD_WEST);
        host->bind(HOS_BIND_PAD, HOS_SW_Y, 0, HOS_PAD_NORTH);
    } else if (layout == 2) { // Swap Face Buttons (Bottom=A, Right=B, Top=X, Left=Y)
        host->bind(HOS_BIND_PAD, HOS_SW_A, 0, HOS_PAD_SOUTH);
        host->bind(HOS_BIND_PAD, HOS_SW_B, 0, HOS_PAD_EAST);
        host->bind(HOS_BIND_PAD, HOS_SW_X, 0, HOS_PAD_NORTH);
        host->bind(HOS_BIND_PAD, HOS_SW_Y, 0, HOS_PAD_WEST);
    }
}
int detect_pad_layout() {
    if (!can_bind()) return opts.pad_layout;
    const int a = host->binding(HOS_BIND_PAD, HOS_SW_A, 0);
    const int b = host->binding(HOS_BIND_PAD, HOS_SW_B, 0);
    const int x = host->binding(HOS_BIND_PAD, HOS_SW_X, 0);
    const int y = host->binding(HOS_BIND_PAD, HOS_SW_Y, 0);
    if (a == HOS_PAD_EAST && b == HOS_PAD_SOUTH && x == HOS_PAD_NORTH && y == HOS_PAD_WEST) return 0;
    if (a == HOS_PAD_SOUTH && b == HOS_PAD_EAST && x == HOS_PAD_WEST && y == HOS_PAD_NORTH) return 1;
    if (a == HOS_PAD_SOUTH && b == HOS_PAD_EAST && x == HOS_PAD_NORTH && y == HOS_PAD_WEST) return 2;
    return 3;
}
// (hookshot) the row's choice to the host: off, the default, or a microphone by its name (and its place in the host's list,
// -1 while it is unplugged: of two with one name, the one chosen). Unplugged, the name is the kept one, cut: the host takes
// a name that starts with it
void mic_tell_host() {
    if (!can_mic()) return;
    if (opts.mic <= 1) host->mic_select(opts.mic <= 0 ? 0 : 1, "", -1);
    else if (mic_missing && opts.mic == 2 + mic_listed) host->mic_select(2, mic_want.c_str(), -1);   // (the kept name, cut)
    else host->mic_select(2, host->mic_name(opts.mic - 2), opts.mic - 2);   // (its whole name: the ini's cut is only the ini's)
}
void mic_row_ends() {
    for (Row& r : rows)
        if (r.value == &opts.mic) r.hi = 1 + mic_listed + (mic_missing ? 1 : 0);
}
// (hookshot) the microphones listed again (one plugged in since shows up), the row on mic_want's: the one at the row's place
// when it still has that name (two microphones of one name: the second stays chosen), else the first of that name, else
// its "(not connected)" entry while it is unplugged
void refresh_mics() {
    if (!can_mic()) {
        if (opts.mic >= 2) opts.mic = 1;
        return;
    }
    mic_listed = std::min(host->mic_count(), kMaxMics);
    int at = -1;
    if (!mic_want.empty()) {
        if (opts.mic >= 2 && opts.mic - 2 < mic_listed && mic_key(host->mic_name(opts.mic - 2)) == mic_want) at = opts.mic - 2;
        for (int i = 0; i < mic_listed && at < 0; i++)
            if (mic_key(host->mic_name(i)) == mic_want) at = i;
        opts.mic = at >= 0 ? at + 2 : 2 + mic_listed;
    } else if (opts.mic >= 2) {
        opts.mic = 1;
    }
    mic_missing = !mic_want.empty() && at < 0;
    mic_row_ends();
    mic_tell_host();
}
// (the row changed by the player: the choice is now this one, by its name; the "(not connected)" entry is kept only while
// it is the one chosen)
void mic_chosen() {
    if (opts.mic >= 2 && !(mic_missing && opts.mic == 2 + mic_listed)) {
        mic_want = mic_key(host->mic_name(opts.mic - 2)), mic_missing = false;
    } else if (opts.mic < 2) {
        mic_want.clear(), mic_missing = false;
    }
    mic_row_ends();
    mic_tell_host();
}
void save() {
    // (hookshot) the lines that are not the menu's are kept: the controls, pad_ and key_ (host/nn_hid.c)
    // (and every other line the menu does not write, such as discord_app_id=: they were dropped at the next save)
    std::string kept, others;
    if (FILE* f = fopen(ini_path().c_str(), "r")) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            if (!strncmp(line, "pad_", 4) || !strncmp(line, "key_", 4)) {
                kept += line;
                continue;
            }
            const char* eq = strchr(line, '=');
            if (!eq || eq == line) continue;
            const std::string key(line, (size_t)(eq - line));
            bool mine = key == "microphone_device" || key == "options_version" || key == "debug_tab";
            for (const Row& r : rows)
                if (!mine && r.value && key == ini_key(r)) mine = true;
            if (!mine) others += line, others += others.back() == '\n' ? "" : "\n";
        }
        fclose(f);
    }
    if (FILE* f = fopen(ini_path().c_str(), "w")) {
        for (const Row& r : rows)
            if (r.value) fprintf(f, "%s=%d\n", ini_key(r), *r.value);
        if (!mic_want.empty()) fprintf(f, "microphone_device=%s\n", mic_want.c_str());   // (hookshot: also while it is unplugged)
        // (2: game_speed below 2 was only ever the old default, see menu_init; 3: the mouse's buttons are bindings, see
        // host/nn_hid.c binds_load; 4: Descriptor reuse on by default, see menu_init and vk_draw.cpp desc_slotgen_on)
        fprintf(f, "options_version=4\n");
        fprintf(f, "debug_tab=%d\n", debug_unlocked ? 1 : 0);   // (Triforce: Alt+G)
        fputs(others.c_str(), f);
        if (!can_bind()) fputs(kept.c_str(), f);
        for (int i = 0; i < HOS_KEY_N && can_bind(); i++) {   // (the keys-only controls, HOS_KEY_*, have no pad_ line)
            if (i < HOS_SW_N) fprintf(f, "pad_%s=%d\n", bind_names[i], host->binding(HOS_BIND_PAD, i, 0));
            fprintf(f, "key_%s=%d,%d\n", bind_names[i], host->binding(HOS_BIND_KEY, i, 0), host->binding(HOS_BIND_KEY, i, 1));
        }
        fclose(f);
    }
}

/// A row's new value v, as the arrows set it (dir: 1 up, -1 down, for the rows that skip or wrap), saved unless !keep
/// (a slider's drag: saved once it is let go)
void set_row(Row& r, int v, int dir, bool keep = true) {
    // (nothing else to choose: no change, no save. A saved choice that cannot run here is kept)
    if (row_fixed(r.value)) return;
    // (the scaling rows: only what can run here; the Render resolution row through the Upscaler's own steps)
    if (r.value == &opts.dlss) v = ur::step_upscaler(upscaler(), dir, upscalers_available());
    if (r.value == &opts.resolution) v = ur::step(upscaler(), *r.value, dir);
    if (r.value == &opts.output_res) {   // (smallest to largest: kOutputOrder)
        v = step_output_choice(*r.value, dir);
        if (v == *r.value) return;
    }
    if (stops_at_ends(r)) v = std::min(std::max(v, r.lo), r.hi);   // (a scale: it stops at its ends)
    if (r.value == &opts.pad_layout) {
        if (v > 2) v = dir > 0 ? 0 : 2;
        if (v < 0) v = 2;
        *r.value = v;
        apply_pad_layout(*r.value);
    } else if (r.value == &opts.preset) {
        *r.value = v > r.hi ? r.lo : v < r.lo ? r.hi : v;
        apply_preset(*r.value);
    } else {
        *r.value = v > r.hi ? r.lo : v < r.lo ? r.hi : v;
        // (another Upscaler: the Render resolution moves to the nearest of its steps, DLSS's modes or the game's)
        if (r.value == &opts.dlss) opts.resolution = ur::snap(upscaler(), opts.resolution);
        if (!r.preset_exempt) {   // (the controls, the window and the counter are not the preset's)
            opts.preset = 0; // Custom
            sync_preset_budgets(0);
        }
    }
    if (r.value == &opts.mic && can_mic()) mic_chosen();   // (hookshot)
    {   // (a row that takes a restart, or a preset that sets one: asked again at the next close)
        int now[mr::kMax];
        restart_values(now);
        restart_state.note(now);
    }
    if (keep) save();
}

// ---- (hookshot) the Controls page: what presses each of the Switch's buttons, a gamepad input and up to two keys ----
struct Control {
    int button;   // HOS_SW_*, or HOS_KEY_* (keys only)
    const char* name;
};
const Control controls[] = {{HOS_SW_A, "A"}, {HOS_SW_B, "B"}, {HOS_SW_X, "X"}, {HOS_SW_Y, "Y"}, {HOS_SW_L, "L"},
                            {HOS_SW_R, "R"}, {HOS_SW_ZL, "ZL"}, {HOS_SW_ZR, "ZR"}, {HOS_SW_PLUS, "Plus"},
                            {HOS_SW_MINUS, "Minus"}, {HOS_SW_LS, "Left stick press"}, {HOS_SW_RS, "Right stick press"},
                            {HOS_SW_UP, "D-pad up"}, {HOS_SW_DOWN, "D-pad down"}, {HOS_SW_LEFT, "D-pad left"},
                            {HOS_SW_RIGHT, "D-pad right"}, {HOS_KEY_MOVE_UP, "Move forward"},
                            {HOS_KEY_MOVE_DOWN, "Move back"}, {HOS_KEY_MOVE_LEFT, "Move left"},
                            {HOS_KEY_MOVE_RIGHT, "Move right"}, {HOS_KEY_CAM_UP, "Camera up"},
                            {HOS_KEY_CAM_DOWN, "Camera down"}, {HOS_KEY_CAM_LEFT, "Camera left"},
                            {HOS_KEY_CAM_RIGHT, "Camera right"}};
constexpr int kControls = sizeof controls / sizeof controls[0];   // (then "Reset to defaults")
bool keys_only(int i) { return i < kControls && controls[i].button >= HOS_SW_N; }   // (no gamepad input for those)
// a pad input's name on the controller of the button prompts (the Button prompts row's choice; Auto: the face in use),
// else by its place
const char* pad_name(int p) {
    static const char* t[HOS_PROMPT_N + 1][HOS_PAD_N] = {   // (by place, then HOS_PROMPT_*)
        {"Bottom button", "Right button", "Left button", "Top button", "Select / Back", "Home", "Start", "Left stick press",
         "Right stick press", "L1 / LB", "R1 / RB", "D-pad up", "D-pad down", "D-pad left", "D-pad right", "L2 / LT", "R2 / RT"},
        {"B", "A", "Y", "X", "Minus", "Home", "Plus", "Left stick press", "Right stick press", "L", "R", "D-pad up",
         "D-pad down", "D-pad left", "D-pad right", "ZL", "ZR"},
        {"A", "B", "X", "Y", "View", "Xbox", "Menu", "Left stick press", "Right stick press", "LB", "RB", "D-pad up",
         "D-pad down", "D-pad left", "D-pad right", "LT", "RT"},
        {"Cross", "Circle", "Square", "Triangle", "Share", "PS", "Options", "L3", "R3", "L1", "R1", "D-pad up", "D-pad down",
         "D-pad left", "D-pad right", "L2", "R2"},
        {"Cross", "Circle", "Square", "Triangle", "Create", "PS", "Options", "L3", "R3", "L1", "R1", "D-pad up", "D-pad down",
         "D-pad left", "D-pad right", "L2", "R2"},
        {"A", "B", "X", "Y", "View", "Steam", "Menu", "L3", "R3", "L1", "R1", "D-pad up", "D-pad down", "D-pad left",
         "D-pad right", "L2", "R2"}};
    const int face = opts.prompts ? opts.prompts - 1 : host && host->prompt_face ? host->prompt_face() : -1;
    return p >= 0 && p < HOS_PAD_N ? t[face >= 0 && face < HOS_PROMPT_N ? face + 1 : 0][p] : "None";
}
const char* key_name(int vk) {
    static char b[16];
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return b[0] = (char)vk, b[1] = 0, b;
    if (vk >= VK_F1 && vk <= VK_F12) return snprintf(b, sizeof b, "F%d", vk - VK_F1 + 1), b;
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD0 + 9) return snprintf(b, sizeof b, "Num %d", vk - VK_NUMPAD0), b;
    switch (vk) {
    case VK_SPACE: return "Space";
    case VK_SHIFT: return "Shift";
    case VK_CONTROL: return "Ctrl";
    case VK_MENU: return "Alt";
    case VK_TAB: return "Tab";
    case VK_RETURN: return "Enter";
    case VK_BACK: return "Backspace";
    case VK_CAPITAL: return "Caps Lock";
    case VK_UP: return "Up";
    case VK_DOWN: return "Down";
    case VK_LEFT: return "Left";
    case VK_RIGHT: return "Right";
    case VK_HOME: return "Home";
    case VK_END: return "End";
    case VK_PRIOR: return "Page Up";
    case VK_NEXT: return "Page Down";
    case VK_ADD: return "Num +";
    case VK_SUBTRACT: return "Num -";
    case VK_MULTIPLY: return "Num *";
    case VK_DIVIDE: return "Num /";
    case VK_DECIMAL: return "Num .";
    case VK_OEM_1: return ";";
    case VK_OEM_PLUS: return "=";
    case VK_OEM_COMMA: return ",";
    case VK_OEM_MINUS: return "-";
    case VK_OEM_PERIOD: return ".";
    case VK_OEM_2: return "/";
    case VK_OEM_3: return "`";
    case VK_OEM_4: return "[";
    case VK_OEM_5: return "\\";
    case VK_OEM_6: return "]";
    case VK_OEM_7: return "'";
    case VK_LBUTTON: return "Left click";
    case VK_RBUTTON: return "Right click";
    case VK_MBUTTON: return "Middle click";
    case 0x05: return "Mouse 4";   // (VK_XBUTTON1)
    case 0x06: return "Mouse 5";
    }
    return snprintf(b, sizeof b, "Key %d", vk), b;
}
// the keys a binding can take (Esc cancels, Insert and F1 are the menu's, Delete clears)
const std::vector<int>& bindable_keys() {
    static std::vector<int> k;
    if (k.empty()) {
        for (int c = 'A'; c <= 'Z'; c++) k.push_back(c);
        for (int c = '0'; c <= '9'; c++) k.push_back(c);
        for (int c = VK_F2; c <= VK_F12; c++) k.push_back(c);
        for (int c = VK_NUMPAD0; c <= VK_NUMPAD0 + 9; c++) k.push_back(c);
        for (int c : {VK_SPACE, VK_SHIFT, VK_CONTROL, VK_MENU, VK_TAB, VK_RETURN, VK_BACK, VK_CAPITAL, VK_UP, VK_DOWN,
                      VK_LEFT, VK_RIGHT, VK_HOME, VK_END, VK_PRIOR, VK_NEXT, VK_ADD, VK_SUBTRACT, VK_MULTIPLY,
                      VK_DIVIDE, VK_DECIMAL, VK_OEM_1, VK_OEM_PLUS, VK_OEM_COMMA, VK_OEM_MINUS, VK_OEM_PERIOD,
                      VK_OEM_2, VK_OEM_3, VK_OEM_4, VK_OEM_5, VK_OEM_6, VK_OEM_7, VK_LBUTTON, VK_RBUTTON, VK_MBUTTON,
                      0x05, 0x06})   // (the mouse's buttons too: they count while the mouse is captured)
            k.push_back(c);
    }
    return k;
}
int page = 0;          // 0 the options, 1 the controls, 3 the teleport list, 4 the inventory, 5 the restart question (2: was the
                       // debug tools' own page, now on the Debug tab)
int csel = 0, ccol = 0;   // the page's row (kTop first: its own rows, then the controls, then reset) and column (0 the
                          // gamepad, 1 the keyboard)
const int kTop = tab_row_count(TAB_PAGE);   // (Layout preset and Button prompts: a preset shows in the grid at once)
int listening = 0;     // waiting for the new input: 1 a gamepad's, 2 a key
bool listen_armed = false;   // (once what started it was let go)
int64_t listen_end = 0;
std::string keys_text(int button) {
    const int a = host->binding(HOS_BIND_KEY, button, 0), b = host->binding(HOS_BIND_KEY, button, 1);
    std::string t = a ? key_name(a) : "";
    if (b) t += std::string(a ? ", " : "") + key_name(b);
    return t.empty() ? "None" : t;
}

// ---- what is drawn: Dear ImGui over the window's image (vk_imgui.h) in the game's own style (vk_ui.h). Built anew at each
// present while something shows, from the menu's state (menu_tick and menu_record run on the presenting thread, one at a time:
// present_image's lock); nothing at all while nothing shows ----
std::mutex startup_mu;   // (the start-up screen's view, set from its own thread)
StartupView startup;
std::atomic<bool> startup_on{false};
std::atomic<float> startup_fade{0};   // (the start-up screen's fade to black at its end, 0 to 1: startup_fade_to)
std::atomic<int64_t> game_frames{0}, shown_frames{0};
char fps_text[48] = "";
char fps_sub[32] = "";
menu_hit::Map hits;      // (the menu's controls as last drawn: the mouse's, menu_tick)
float scroll_at[TAB_MOTION + 2];   // (the first row shown, eased toward the selection: the options' tabs and the Motion page, then
                                   // the Controls page: kControlsSlot)
constexpr int kControlsSlot = TAB_MOTION + 1;
int wheel_rows[TAB_MOTION + 2];   // (the mouse wheel's notches since the last drawing, a slot each: scroll_rows scrolls by them)
int scrolled_to[TAB_MOTION + 2] = {-1, -1, -1, -1, -1, -1, -1, -1};   // (the selection scroll_rows last brought into view)
static_assert(TAB_MOTION + 2 == 8, "scrolled_to: a slot for each tab, the Motion page and the Controls page");
float scroll_want[TAB_MOTION + 2];   // (where scroll_at is going: the wheel moves it, a new selection too)
float ease_step = 0;     // (this drawing's share of the way: the time since the last one)

ui::Ctx ui_ctx(uint32_t w, uint32_t h) {
    ui::Ctx c{ImGui::GetForegroundDrawList(), ui::fonts(), 1, (float)w, (float)h, 0, pad_last};
    c.confirm_dot = menu_keys::dot(pad_confirm), c.back_dot = menu_keys::dot(pad_back);
    c.s = ui::unit((float)h, 0, false, false, &c.safe);
    // (the cursor's glow breathes, as the game's: a 2 s cycle; RX_VK_UI_STILL=1 holds it, for pictures that compare)
    static const bool still = getenv("RX_VK_UI_STILL") != nullptr;
    c.pulse = still ? 0.6f : 0.5f + 0.5f * std::sin((float)((prof_now() % 2000000000LL) / 2e9 * 6.2831853));
    return c;
}
/// (Triforce) The menu's, the counter's and the notice's: the game's menu fonts where the pages draw their text (body its
/// "Normal", strong its "Special"); the start-up screen keeps ui_ctx's
ui::Ctx menu_ctx(uint32_t w, uint32_t h) {
    ui::Ctx c = ui_ctx(w, h);
    if (c.f.menu) c.f.body = c.f.menu, c.f.strong = c.f.heading;
    return c;
}
/// The header's height (SystemOverlay_00's P_SystemHeader_00: 200 units)
float banner_h(const ui::Ctx& c) { return ui::banner_height(c); }
// (the caption's texts: PaGuideKey's 48 units for the help, a little smaller for the line about the value)
// (the game's caption is 54 units, 65 as drawn; ours say more than its one line, so a little smaller: 57 and 50)
constexpr float kHelpPt = 19.0f, kHintPt = 16.6f;

/// The tallest help a row of the options can have, in pixels at width: its help and the tallest line any of its values can
/// have: the help box keeps that height whichever row is selected and whatever its value
float measure_help(ui::Ctx& c, float width) {
    float most = 0;
    for (const Row& r : rows) {
        float hint = 0;
        for (int v = r.lo; r.value && r.hint && v <= r.hi; v++)
            for (int k = 0; const char* t = r.hint(v, k); k++)
                hint = std::max(hint, ui::para(c, c.f.body, kHintPt * c.s, ImVec2(), 0, t, width - 18 * c.s, false));
        if (r.help) most = std::max(most, ui::para(c, c.f.body, kHelpPt * c.s, ImVec2(), 0, r.help, width, false) + (hint ? 14 * c.s + hint : 0));
    }
    return most;
}
/// label in room pixels: as it is when it fits; the selected row's slides through it (marquee_pos, marquee_max for menu_tick);
/// another's ends with "..."
/// (UTF-8: the byte offsets where its characters start, and the end; labels are cut and slid a character at a time, never
/// inside one: a microphone's name can be in any script)
std::vector<size_t> char_starts(const std::string& t) {
    std::vector<size_t> at;
    for (size_t i = 0; i < t.size(); i++)
        if (((unsigned char)t[i] & 0xC0) != 0x80) at.push_back(i);
    at.push_back(t.size());
    return at;
}
std::string fit(ui::Ctx& c, ImFont* f, float px, float room, const std::string& label, bool sel) {
    auto width = [&](const std::string& t) { return ui::size_of(f, px, t.c_str()).x; };
    if (width(label) <= room) return label;
    const std::vector<size_t> at = char_starts(label);
    const int chars = (int)at.size() - 1;
    if (sel) {   // (marquee_pos and marquee_max count characters)
        int start = chars;   // (the last start that still shows the label's end)
        while (start > 0 && width(label.substr(at[start - 1])) <= room) start--;
        marquee_max = start;
        const int from = std::min(marquee_pos, marquee_max);
        int end = chars;
        while (end > from + 1 && width(label.substr(at[from], at[end] - at[from])) > room) end--;
        return label.substr(at[from], at[end] - at[from]);
    }
    int end = chars;
    while (end > 1 && width(label.substr(0, at[end]) + "...") > room) end--;
    std::string t = label.substr(0, at[end]);
    while (!t.empty() && t.back() == ' ') t.pop_back();
    return t + "...";
}
/// A list of rows from top to bottom (pixels), n of them pitch apart, the selection sel kept in view: the first one shown,
/// eased toward its place (scroll_at[slot]); *shown how many fit
float scroll_rows(int slot, int n, int sel, float top, float bottom, float pitch, int* shown) {
    const int fit = std::max(1, (int)((bottom - top) / pitch));
    *shown = fit;
    float& at = scroll_at[slot];
    float& want = scroll_want[slot];
    const int wheel = std::exchange(wheel_rows[slot], 0);
    if (n <= fit) want = 0;   // (nothing to scroll: the wheel does nothing)
    else {
        want -= (float)wheel;   // (the wheel scrolls the rows and leaves the selection, and every value, as they are)
        static int fits[sizeof scroll_at / sizeof *scroll_at];   // (and after a resize: the rows that fit changed)
        if (sel != scrolled_to[slot] || fit != std::exchange(fits[slot], fit)) {   // (a new selection, by the keys or the pad: brought into view)
            const int margin = fit > 4 ? 1 : 0;   // (a row of context above and below the selection)
            if (sel - margin < want) want = (float)(sel - margin);
            if (sel + margin + 1 > want + fit) want = (float)(sel + margin + 1 - fit);
        }
        want = std::round(std::clamp(want, 0.0f, (float)(n - fit)));
    }
    scrolled_to[slot] = sel;
    at += (want - at) * ease_step;
    if (std::fabs(want - at) < 0.01f) at = want;
    return at;
}
/// The scroll bar at x, beside a list of n rows of which shown fit, the first at `first`: the game's (PaScrollBar_00: a short
/// bar, 32 x 312 units, at the list's middle; its lit part the rows in view)
void scroll_bar(ui::Ctx& c, float x, float top, float bottom, int n, int shown, float first) {
    if (n <= shown && !c.over_game) return;   // (over the game's own list its bar is under ours: drawn, whole, even so)
    if (n <= shown) n = shown = 1, first = 0;
    const float mid = (top + bottom) * 0.5f, half = ui::lu(c, 156), w = ui::lu(c, 16);
    const float y0 = mid - half + 2 * half * first / n, y1 = mid - half + 2 * half * (first + shown) / n;
    if (const ui::Art* t = ui::art("ScrollBarBase_00")) {
        ui::image(c, t, ImVec2(x - w, mid - half), ImVec2(x + w, mid + half), ui::rgba(18, 14, 9, 235));
        c.d->PushClipRect(ImVec2(x - w, y0), ImVec2(x + w, y1), true);
        ui::image(c, t, ImVec2(x - w, mid - half), ImVec2(x + w, mid + half), ui::rgba(200, 146, 66));
        c.d->PopClipRect();
        return;
    }
    c.d->AddRectFilled(ImVec2(x - w * 0.4f, mid - half), ImVec2(x + w * 0.4f, mid + half), ui::rgba(18, 14, 9, 235), w * 0.4f);
    c.d->AddRectFilled(ImVec2(x - w * 0.4f, y0), ImVec2(x + w * 0.4f, y1), ui::rgba(200, 146, 66), w * 0.4f);
}
/// The header with a page's title in the middle (the Controls, Motion and list pages: as the game's "Load"), its height
float title_banner(ui::Ctx& c, const char* title, const char* right) {
    ui::banner(c, 0, title);
    if (right) ui::text_mid_right(c, c.f.body, ui::lu(c, 50), ui::at(c, 1800, 0).x, ui::at(c, 0, 990).y, ui::kBannerInk, right);
    return banner_h(c);
}
/// The caption plate from (x0, y0) to x1 (what shows of it): a title when the page has one, the help, a line about the value
/// set off by a bar, a line at its foot; the text in the plate's middle, as the game sets its caption. min_h: its text's
/// room at least; kept between y_min and y_max. bleed: how far the plate goes on to the right of x1, off the screen (the
/// game's caption plate does)
void help_box(ui::Ctx& c, float x0, float x1, float y0, const char* title, const char* help, const char* hint, const char* gold_line,
              float min_h = 0, bool after_restart = false, float y_min = 0, float y_max = FLT_MAX, float bleed = 0) {
    const float s = c.s, pad_x = ui::lu(c, 72), pad_y = ui::lu(c, 56), w = x1 - x0 - 2 * pad_x;
    const float th = help ? ui::para(c, c.f.body, kHelpPt * s, ImVec2(), 0, help, w, false) : 0,
                hh = hint ? ui::para(c, c.f.body, kHintPt * s, ImVec2(), 0, hint, w - 18 * s, false) : 0;
    const float text_h = th + (hint ? 14 * s + hh : 0), note = after_restart ? 30 * s : 0;
    const float content = (title ? 34 * s : 0) + note + text_h + (gold_line ? 46 * s : 0);
    float tall = std::max(min_h, content + 2 * pad_y);
    if (y0 + tall > y_max) y0 = std::max(y_min, y_max - tall);
    const float y1 = std::min(y0 + tall, y_max);
    ui::box(c, ImVec2(x0, y0), ImVec2(x1 + bleed, y1));
    c.d->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);
    float y = std::max(y0 + pad_y * 0.5f, (y0 + y1 - content) * 0.5f);
    if (title) ui::text(c, c.f.strong, 21.6f * s, ImVec2(x0 + pad_x, y), ui::kInk, title), y += 34 * s;
    if (after_restart) {   // (this row's change applies at the next start: said here, not in the row, where it would cut the name)
        ui::diamond(c, ImVec2(x0 + pad_x + 5 * s, y + 10 * s), 4 * s, ui::kGoldHi);
        ui::text_mid(c, c.f.strong, kHintPt * s, x0 + pad_x + 16 * s, y + 10 * s, ui::kGoldHi, "Takes effect after a restart");
        y += note;
    }
    if (help) ui::para(c, c.f.body, kHelpPt * s, ImVec2(x0 + pad_x, y), ui::kInk, help, w);
    y += th;
    if (hint) {
        const float hy = y + 14 * s;
        c.d->AddRectFilled(ImVec2(x0 + pad_x, hy + 2 * s), ImVec2(x0 + pad_x + 2.5f * s, hy + hh - 2 * s), ui::kAccent, s);
        ui::para(c, c.f.body, kHintPt * s, ImVec2(x0 + pad_x + 14 * s, hy), ui::kSoft, hint, w - 18 * s);
        y = hy + hh;
    }
    if (gold_line) {
        y += 14 * s;
        c.d->AddLine(ImVec2(x0 + pad_x, y), ImVec2(x1 - pad_x, y), ui::alpha(ui::kEdge, 0.8f), std::max(1.0f, s));
        ui::diamond(c, ImVec2(x0 + pad_x + 5 * s, y + 18 * s), 4 * s, ui::kGoldHi);
        ui::text_mid(c, c.f.strong, kHintPt * s, x0 + pad_x + 16 * s, y + 18 * s, ui::kGoldHi, gold_line);
    }
    c.d->PopClipRect();
}
/// The caption of a page at the options' place (SystemOption_00's N_Caption: its plate 90 units under the screen's middle, off the
/// screen's right edge), as tall as its text asks, between the header and the guides
void caption(ui::Ctx& c, float hx0, float hx1, const char* title, const char* help, const char* hint, const char* gold_line, float min_h = 0,
             bool after_restart = false);
/// The game's Options layout (SystemOption_00): the list's column 2240 units wide, its middle 730 left of the screen's (drawn by
/// draw_menu, under the header); the caption plate to its right, what shows of it from 440 to the screen's edge
constexpr float kListX = -730;   // (N_List_00)
void columns(const ui::Ctx& c, float* x0, float* x1, float* hx0, float* hx1, bool* narrow) {
    *x0 = ui::at(c, kListX - 1120, 0).x, *x1 = ui::at(c, kListX + 1120, 0).x;
    *hx0 = ui::at(c, 456, 0).x, *hx1 = ui::at(c, 1850, 0).x;
    *narrow = false;
}
void column(ui::Ctx&, float, float, float) {}   // (the column is under the header: draw_menu draws it first)
void caption(ui::Ctx& c, float hx0, float hx1, const char* title, const char* help, const char* hint, const char* gold_line, float min_h,
             bool after_restart) {
    // (the game's: 476 units of plate about a line 90 under the screen's middle; taller, about the same line, for a long text)
    const float s = c.s, hh = banner_h(c), bar_top = ui::prompt_bar_top(c), w = hx1 - hx0 - 2 * ui::lu(c, 72);
    const float th = help ? ui::para(c, c.f.body, kHelpPt * s, ImVec2(), 0, help, w, false) : 0,
                nh = hint ? 14 * s + ui::para(c, c.f.body, kHintPt * s, ImVec2(), 0, hint, w - 18 * s, false) : 0;
    const float content = std::max(min_h, th + nh) + (title ? 34 * s : 0) + (after_restart ? 30 * s : 0) + (gold_line ? 46 * s : 0);
    const float tall = std::max(ui::lu(c, 476), content + 2 * ui::lu(c, 56)), mid = ui::at(c, 0, -90).y;
    const float y0 = std::max(hh + 14 * s, mid - tall * 0.5f);
    help_box(c, hx0, hx1, y0, title, help, hint, gold_line, tall, after_restart, hh + 14 * s, bar_top - 8 * s, ui::lu(c, 330));
}
/// A row's name on its plate; pending: a change that applies after a restart, the game's corner mark at its top left in
/// gold (the help says it in words). The selected row's plate is as the others': the game's cursor alone marks it
void name_cell(ui::Ctx& c, ImVec2 a, ImVec2 b, const char* name, bool sel, bool pending = false, float px = 18.0f * ui::kType) {
    const float s = c.s, mid = (a.y + b.y) * 0.5f;
    (void)sel;
    ui::plate(c, a, b);
    if (pending) {
        if (const ui::Art* t = ui::art("OptionCheckMark_00")) ui::image(c, t, a + ImVec2(ui::lu(c, -8), ui::lu(c, -8)), a + ImVec2(ui::lu(c, 82), ui::lu(c, 82)), ui::kGoldHi);
        else ui::diamond(c, ImVec2(a.x + 10 * s, a.y + 10 * s), 4 * s, ui::kGoldHi);
    }
    const float room = b.x - a.x - ui::lu(c, 110);
    const std::string shown = fit(c, c.f.body, px * s, room, name, false);
    ui::text_mid(c, c.f.body, px * s, a.x + ui::lu(c, 72), mid - ui::lu(c, 2), ui::kInk, shown.c_str());
}
/// A value, as the game's choices: the near-black panel a..b, on it the orange plate with the value in white and the game's
/// small arrows at its ends (left, right: whether it can go that way); the cursor round the panel when selected. fixed:
/// nothing else to choose here, dim. plain: no plate (a row that opens a page: its word and an arrow)
void choice_cell(ui::Ctx& c, ImVec2 a, ImVec2 b, const std::string& value, bool sel, bool left, bool right, bool fixed = false,
                 bool plain = false) {
    const float mid = (a.y + b.y) * 0.5f, px = std::min(ui::lu(c, 54 * ui::kType), (b.y - a.y) * 0.62f), in = ui::lu(c, 8);
    ui::value_plate(c, a, b);
    if (!plain) ui::value_plate(c, a + ImVec2(in, in), b - ImVec2(in, in), true, fixed ? 0.30f : 1.0f);
    const ImU32 ink = fixed ? ui::kDim : ui::kInk;
    if (left) ui::arrow(c, ImVec2(a.x + ui::lu(c, 40), mid), false, ink);
    if (right) ui::arrow(c, ImVec2(b.x - ui::lu(c, 40), mid), true, ink);
    const std::string shown = fit(c, c.f.body, px, b.x - a.x - ui::lu(c, left || right ? 150 : 50), value, sel);   // (room for arrows)
    ui::text_mid_centre(c, c.f.body, px, (a.x + b.x) * 0.5f, mid - ui::lu(c, 2), ink, shown.c_str());
    if (sel) ui::cursor(c, a - ImVec2(ui::lu(c, 2.5f), ui::lu(c, 3)), b + ImVec2(ui::lu(c, 2.5f), ui::lu(c, 3)));
}
/// A value of two halves, as the game's toggles (On at the left, Off at the right): the one chosen on its orange plate in
/// white, the other dim on the panel; the left one's plate the wider (the layout's 516 and 320 of 836). high: the right
/// one is chosen
void halves_cell(ui::Ctx& c, ImVec2 a, ImVec2 b, const char* lo, const char* hi, bool high, bool sel, bool fixed) {
    const float mid = (a.y + b.y) * 0.5f, in = ui::lu(c, 8), px = std::min(ui::lu(c, 54 * ui::kType), (b.y - a.y) * 0.62f);
    const float xm = a.x + (b.x - a.x) * 0.632f;   // (where the two meet: 606 of the panel's 98 to 902)
    ui::value_plate(c, a, b);
    const ImVec2 fa = high ? ImVec2(xm, a.y + in) : a + ImVec2(in, in), fb = high ? b - ImVec2(in, in) : ImVec2(xm, b.y - in);
    ui::value_plate(c, fa, fb, true, fixed ? 0.30f : 1.0f);
    const ImU32 on = fixed ? ui::kDim : ui::kInk;
    ui::text_mid_centre(c, c.f.body, px, (a.x + xm) * 0.5f, mid - ui::lu(c, 2), high ? ui::kDim : on, lo);
    ui::text_mid_centre(c, c.f.body, px, (xm + b.x) * 0.5f, mid - ui::lu(c, 2), high ? on : ui::kDim, hi);
    if (sel) ui::cursor(c, a - ImVec2(ui::lu(c, 2.5f), ui::lu(c, 3)), b + ImVec2(ui::lu(c, 2.5f), ui::lu(c, 3)));
}
/// A scale's value: as a choice with its arrows (the game's Options have no sliders: its levels are choices too), a fine
/// line under the value for how far along it is; *t0, *t1 the line (for the mouse)
void slider_cell(ui::Ctx& c, ImVec2 a, ImVec2 b, const char* value, int permille, bool sel, float* t0, float* t1) {
    choice_cell(c, a, b, value, sel, permille > 0, permille < 1000);
    const float x0 = a.x + ui::lu(c, 150), x1 = b.x - ui::lu(c, 150), y = b.y - ui::lu(c, 22), h = std::max(1.0f, ui::lu(c, 2.5f));
    c.d->AddRectFilled(ImVec2(x0, y - h), ImVec2(x1, y + h), ui::rgba(60, 36, 8, 110), h);
    c.d->AddRectFilled(ImVec2(x0, y - h), ImVec2(x0 + (x1 - x0) * permille / 1000.0f, y + h), ui::kInk, h);
    *t0 = x0, *t1 = x1;
}
/// "Restart needed: " and the pending rows, on one line of width: the last ones as "and N more" when they do not fit
std::string restart_line(ui::Ctx& c, float px, float width) {
    const unsigned all = restart_pending();
    std::string line;
    for (int keep = restart_state.n; keep >= 0; keep--) {   // (the first `keep` pending rows by name)
        unsigned shown = 0;
        int named = 0, more = 0;
        for (int i = 0; i < restart_state.n; i++)
            if ((all >> i) & 1) (named < keep ? (shown |= 1u << i, named++) : more++);
        line = "Restart needed: " + mr::names(restart_names, restart_state.n, shown);
        if (more) line += (named ? " and " : "") + std::to_string(more) + (named ? " more" : more > 1 ? " settings" : " setting");
        if (ui::size_of(c.f.strong, px, line.c_str()).x <= width) break;
    }
    return line;
}

/// (hookshot) The Debug tab's order: the tools' rows by their group (Row::cheat), Up and Down walking them so; Inventory item
/// and Change quantity are not shown (the Inventory page edits every amount)
struct DebugGroup { const char* title; int from, to; };   // (debug_tools_order[from, to))
constexpr int debug_tools_order[] = {0, 1, 9, 10, 8, 2, 3, 7, 4};
constexpr DebugGroup debug_groups[] = {{"Player", 0, 4}, {"Travel", 4, 7}, {"Camera & world", 7, 9}};
constexpr int kDebugGroups = sizeof debug_groups / sizeof debug_groups[0];
constexpr int kDebugTools = sizeof debug_tools_order / sizeof debug_tools_order[0];
/// The Debug tab's rows as shown (its row numbers, tab_row_to_global's): the switch, the tools in debug_tools_order while they
/// are on, then the Developer rows; *dev where those start, *dev_head the Developer heading's row (-1 none). Their count
int debug_rows(int* out, int* dev, int* dev_head) {
    const int count = tab_row_count(TAB_DEBUG);
    int n = 0;
    *dev_head = -1;
    for (int k = 0; k < count; ++k)
        if (rows[tab_row_to_global(TAB_DEBUG, k)].value == &opts.debug_tools) out[n++] = k;
    for (int t = 0; t < kDebugTools; ++t)
        for (int k = 0; k < count; ++k)
            if (rows[tab_row_to_global(TAB_DEBUG, k)].cheat == debug_tools_order[t]) out[n++] = k;
    *dev = n;
    for (int k = 0; k < count; ++k) {
        const Row& r = rows[tab_row_to_global(TAB_DEBUG, k)];
        if (r.heading) *dev_head = k;
        else if (*dev_head >= 0 && r.cheat < 0 && r.value != &opts.debug_tools) out[n++] = k;
    }
    return n;
}
/// The Debug tab's row after sel (dir 1) or before it (dir -1), in debug_rows' order
int debug_step(int sel, int dir) {
    int order[64], dev = 0, head = -1;
    const int n = debug_rows(order, &dev, &head);
    int at = 0;
    while (at < n && order[at] != sel) at++;
    if (at == n) return order[0];
    return order[(at + n + dir) % n];
}
/// (hookshot) The Debug tab's rows from top down to bottom, sel selected: Debug tools across the top; the tools under their
/// headings, Player in the first column and Travel and Camera & world in the second when two fit (else one under another);
/// the Developer rows across the foot. Each value cell a column's width, so their edges line up. The hits are the tab's row
/// numbers, as the list's: menu_tick's clicks and keys are the other tabs'
void draw_debug_rows(ui::Ctx& c, float cx0, float cx1, float top, float bottom, int sel) {
    const float s = c.s, px = 18 * s, pitch = 48 * s, rh = 40 * s, gap = 24 * s;   // (the other tabs' rows: 40 high, 48 apart)
    const float left = cx0 + 44 * s, right = cx1 - 44 * s, mid = (left + right) * .5f;
    int order[64], dev = 0, dev_head = -1;
    const int n = debug_rows(order, &dev, &dev_head);
    // (two columns only when each fits its longest name whole (name_cell's room, the selected's bold) and its value cells at
    // their narrowest; the cells then as wide as the room lets them, up to what they ask)
    const float halves_w = 2 * std::max(ui::size_of(c.f.strong, px, "Off").x, ui::size_of(c.f.strong, px, "On").x);
    auto need = [&](const Row& r, bool tight) {
        const float pad = (tight ? 36 : 64) * s;
        if (r.cheat == 9 && age_shown(9))
            return 2 * std::max(ui::size_of(c.f.strong, px, "Kid").x, ui::size_of(c.f.strong, px, "Adult").x) + pad;
        if (r.cheat == 0 || r.cheat == 1 || r.cheat == 7 || r.value) return halves_w + pad;
        const std::string v = cheat_value(r.cheat);
        const char* const says[kCheatRows] = {"", "", "Mark", "Return", "", "", "", "", "Choose map", "Switch", "Edit items"};
        return ui::size_of(c.f.strong, px, v.empty() ? says[r.cheat] : v.c_str()).x + (cheat_chooses(r.cheat) ? 68 : tight ? 56 : 60) * s;   // (choice_cell keeps 48 for arrows either way)
    };
    float name_need[2] = {0, 0}, cell_full[2] = {halves_w + 64 * s, halves_w + 64 * s}, cell_tight[2] = {halves_w + 36 * s, halves_w + 36 * s};
    for (int i = 1; i < dev; ++i) {
        const Row& r = rows[tab_row_to_global(TAB_DEBUG, order[i])];
        const int col = i - 1 >= debug_groups[0].to ? 1 : 0;   // (Player in the first, the rest in the second)
        name_need[col] = std::max(name_need[col], ui::size_of(c.f.strong, 18.5f * s, r.name).x + 40 * s);
        cell_full[col] = std::max(cell_full[col], need(r, false)), cell_tight[col] = std::max(cell_tight[col], need(r, true));
    }
    const float col_w = (right - left - gap) * .5f;
    const bool two = col_w >= name_need[0] + cell_tight[0] && col_w >= name_need[1] + cell_tight[1];
    // (where each goes: a row number of the layout and a column)
    int vrow[64] = {}, vcol[64] = {}, head_row[kDebugGroups] = {}, head_col[kDebugGroups] = {};
    int next_row[2] = {1, 1};
    const bool tools = dev > 1;
    for (int g = 0; tools && g < kDebugGroups; ++g) {
        const int col = two && g > 0 ? 1 : 0;
        if (next_row[col] > 1) next_row[col]++;   // (a gap over a heading that follows another group)
        head_row[g] = next_row[col]++, head_col[g] = col;
        for (int t = debug_groups[g].from; t < debug_groups[g].to; ++t) vrow[1 + t] = next_row[col]++, vcol[1 + t] = col;
    }
    int foot = std::max(next_row[0], next_row[1]);
    if (tools) foot++;
    const int dev_row = foot;
    for (int i = dev; i < n; ++i) vrow[i] = dev_row + 1 + (i - dev);
    const int visual_count = n > dev ? dev_row + 1 + (n - dev) : foot;
    int sel_at = 0;
    for (int i = 0; i < n; ++i)
        if (order[i] == sel) sel_at = vrow[i];
    int shown = 0;
    const float first = scroll_rows(TAB_DEBUG, visual_count, sel_at, top, bottom, pitch, &shown);
    scroll_bar(c, cx0 + 20 * s, top, top + shown * pitch - (pitch - rh), visual_count, shown, first);
    // (the value cells' widths, a column's alike: what they ask, given up toward their narrowest while a name needs the room)
    float cell_w[2];
    if (two) {
        for (int k = 0; k < 2; ++k) cell_w[k] = std::max(cell_tight[k], std::min(cell_full[k], col_w - name_need[k]));
    } else {   // (one column: as wide as any of the tools asks)
        const float w = std::max(cell_full[0], cell_full[1]);
        cell_w[0] = cell_w[1] = std::max(std::min(w, right - left - std::max(name_need[0], name_need[1])), std::max(cell_tight[0], cell_tight[1]));
    }
    auto heading = [&](float x0, float x1, int row, const char* title) {   // (as the list's Developer heading: gold, a rule after)
        const float y = top + (row - first) * pitch, ty = y + rh * 0.62f, tw = ui::size_of(c.f.heading, 19 * s, title).x;
        if (y + pitch <= top || y >= bottom) return;
        ui::text_mid(c, c.f.heading, 19 * s, x0 + 2 * s, ty, ui::kGoldHi, title);
        if (x0 + tw + 16 * s < x1) c.d->AddLine(ImVec2(x0 + tw + 16 * s, ty), ImVec2(x1, ty), ui::alpha(ui::kEdge, 0.9f), std::max(1.0f, s));
    };
    c.d->PushClipRect(ImVec2(cx0, top), ImVec2(cx1, std::max(top, bottom)), true);
    for (int g = 0; tools && g < kDebugGroups; ++g)
        heading(head_col[g] ? mid + gap * .5f : left, !two || head_col[g] ? right : mid - gap * .5f, head_row[g], debug_groups[g].title);
    if (dev_head >= 0 && n > dev) heading(left, right, dev_row, rows[tab_row_to_global(TAB_DEBUG, dev_head)].name);
    const char* const buttons[kCheatRows] = {"", "", "Mark", "Return", "", "", "", "", "Choose map", "Switch", "Edit items"};
    for (int i = 0; i < n; ++i) {
        const int k = order[i];
        const Row& r = rows[tab_row_to_global(TAB_DEBUG, k)];
        const float y = top + (vrow[i] - first) * pitch;
        if (y + pitch <= top || y >= bottom) continue;
        const bool in_tools = i > 0 && i < dev;
        const float x0 = two && in_tools && vcol[i] ? mid + gap * .5f : left;
        const float x1 = !two || !in_tools || vcol[i] ? right : mid - gap * .5f;
        const bool selected = k == sel;
        const float width = in_tools ? cell_w[vcol[i]] : std::max(need(r, false), cell_w[two ? 1 : 0]);
        const ImVec2 va(x1 - width, y), vb(x1, y + rh);
        name_cell(c, ImVec2(x0, y), ImVec2(va.x, y + rh), r.name, selected, restart_changed(r));   // (boxed, as on the other tabs)
        if (r.value) {   // (Debug tools and the Developer rows: values of the options, Off and On)
            halves_cell(c, va, vb, std::string(r.text(r.hi)).c_str(), std::string(r.text(r.lo)).c_str(), *r.value == r.lo, selected,
                        row_fixed(r.value));
        } else if (r.cheat == 0 || r.cheat == 1 || r.cheat == 7) {
            halves_cell(c, va, vb, "On", "Off", cheat_value(r.cheat) != "On", selected, false);
        } else if (age_shown(r.cheat)) {   // (Link's age now: the other half switches)
            halves_cell(c, va, vb, "Kid", "Adult", link_age.load(std::memory_order_relaxed) == 1, selected, false);
        } else {
            const std::string v = cheat_value(r.cheat);
            choice_cell(c, va, vb, v.empty() ? buttons[r.cheat] : v, selected, cheat_chooses(r.cheat), cheat_chooses(r.cheat), r.cheat == 9);
        }
        hits.add(x0, std::max(y, top), x1, std::min(y + rh, bottom), menu_hit::kRow, k, va.x, vb.x);
    }
    c.d->PopClipRect();
}

/// The tabs across the header (as the pause menu's categories: MenuOverlay_00, 460 units apart, L and R at their ends; closer
/// with more than five). sel: the one shown, -1 the game's own. game: "Game" in front of ours, inside the game's Options
/// screen. hit: the mouse's places noted
void draw_tabs(ui::Ctx& c, int sel, bool game, bool hit) {
    static const char* const ours[TAB_COUNT] = {"Display", "Graphics", "Audio", "Controls", "System", "Debug"};
    const float s = c.s, hh = banner_h(c);
    const int n_ours = tabs_shown(), tabs = n_ours + (game ? 1 : 0);
    const float ty = ui::at(c, 0, 998).y, step = ui::lu(c, tabs > 6 ? 350 : tabs > 5 ? 400 : 460), tpx = ui::lu(c, 54 * ui::kType);
    const float tx0 = c.w * 0.5f - step * (tabs - 1) * 0.5f;
    for (int i = 0; i < tabs; i++) {
        const int tab = game ? i - 1 : i;   // (-1: the game's own)
        const float x = tx0 + i * step;
        const bool shown = tab == sel;
        if (shown) {   // (the chosen category: the pause menu's own plate behind its name, MenuHeaderSelect: PaMenuCategory's
            // 540 x 260 pane, 34 units under the header's middle line, in the brown the pause menu's System tab has)
            const ImVec2 m = ui::at(c, 0, 966);
            const float hw = std::min(ui::lu(c, 270), step * 0.5f + ui::lu(c, 40));
            if (const ui::Art* t = ui::art("MenuHeaderSelect_00")) ui::image(c, t, ImVec2(x - hw, m.y - ui::lu(c, 130)), ImVec2(x + hw, m.y + ui::lu(c, 130)), ui::rgba(190, 125, 62));
            else c.d->AddRectFilled(ImVec2(x - step * 0.5f, 0), ImVec2(x + step * 0.5f, hh + ui::lu(c, 6)), ui::kTabInset);
        } else if (hit && tab >= 0 && tab == hover_tab) {
            c.d->AddRectFilled(ImVec2(x - step * 0.5f + 4 * s, ty - ui::lu(c, 60)), ImVec2(x + step * 0.5f - 4 * s, ty + ui::lu(c, 60)), ui::rgba(60, 42, 22, 70), 6 * s);
        }
        ui::text_mid_centre(c, c.f.strong, tpx, x, ty, ui::kInk, tab < 0 ? "Game" : ours[tab]);
        if (hit) hits.add(x - step * 0.5f, 0, x + step * 0.5f, hh, menu_hit::kTab, tab < 0 ? kGameTab : tab);
    }
    for (int i = 0; i < 2; i++) {   // (L and R at the tabs' ends, the game's icons: MenuOverlay_00's T_BtnL and T_BtnR, 90 units)
        const float x = i ? tx0 + (tabs - 1) * step + ui::lu(c, 298) : tx0 - ui::lu(c, 298);
        if (c.pad && c.f.glyph) {
            const char* code = i ? ui::kIconR : ui::kIconL;
            const ImVec2 sz = ui::size_of(c.f.glyph, ui::lu(c, 90), code);
            c.d->AddText(c.f.glyph, ui::lu(c, 90), ImVec2(x - sz.x * 0.5f, ty - sz.y * 0.5f + ui::lu(c, 2)), ui::kInk, code);
        } else {   // (the keys: Q and E; inside the game's Options the keys bound to the Switch's L and R, which are its own there)
            std::string label = c.pad ? (i ? "R" : "L") : (i ? "E" : "Q");
            if (!c.pad && game && can_bind())
                if (const int vk = host->binding(HOS_BIND_KEY, i ? HOS_SW_R : HOS_SW_L, 0)) label = key_name(vk);
            const float kh = ui::lu(c, 62), kw = std::max(ui::lu(c, 62), ui::size_of(c.f.strong, ui::lu(c, 40), label.c_str()).x + ui::lu(c, 30));
            c.d->AddRectFilled(ImVec2(x - kw * 0.5f, ty - kh * 0.5f), ImVec2(x + kw * 0.5f, ty + kh * 0.5f), ui::kInk, ui::lu(c, 14));
            ui::text_mid_centre(c, c.f.strong, ui::lu(c, 40), x, ty, ui::rgba(40, 30, 20), label.c_str());
        }
        if (hit) hits.add(i ? x - ui::lu(c, 90) : 0, 0, i ? c.w : x + ui::lu(c, 90), hh, i ? menu_hit::kTabNext : menu_hit::kTabPrev, -1);
    }
}

/// The Options screen: the header with the tabs across it (as the pause menu's categories: MenuOverlay_00, 460 units apart,
/// L and R at their ends), the rows in the list's column (SystemOption_00 and its rows' layouts: a row 1836 x 136 units, 170
/// apart, its name 840 left of the list's middle, its value's plate 820 x 116 at 500 right of it, the cursor round the
/// value), the caption plate at the right with what the selected row is, the guides at the foot
void draw_options(ui::Ctx& c) {
    const float s = c.s, hh = banner_h(c);
    ui::banner(c, hh);
    draw_tabs(c, cur_tab == TAB_MOTION ? TAB_CONTROLS : cur_tab, embed_open, true);   // (the Motion page: under Controls)
    // the rows, in the column; the caption beside it
    float cx0, cx1, hx0, hx1;
    bool narrow;
    columns(c, &cx0, &cx1, &hx0, &hx1, &narrow);
    const int count = tab_row_count(cur_tab), sel = tab_selected[cur_tab];
    const Row& selr = rows[tab_row_to_global(cur_tab, sel)];
    const char* hint = selr.value && selr.hint ? selr.hint(*selr.value, -1) : nullptr;
    const float cap_w = hx1 - hx0 - 2 * ui::lu(c, 76);
    const std::string pending_line = restart_pending() ? restart_line(c, kHintPt * s, cap_w - 20 * s) : "";
    static float help_tall = 0, help_tall_w = 0;
    if (help_tall_w != cap_w) help_tall_w = cap_w, help_tall = measure_help(c, cap_w);
    // (the guides: what the buttons do on this row, as the game's own say only that)
    const bool confirms = &selr == &rows[kRestartRow] || &selr == &rows[kQuitRow];
    ui::Prompt prompt_list[5] = {{ui::kLeftRight, "Change"}, {ui::kTabs, "Category"}, {ui::kBack, "Back"}};
    int prompts_n = 3;
    if (!selr.value) prompt_list[0] = {ui::kConfirm, confirms ? (pending == &selr ? "Confirm" : selr.name) : "Open"};
    if (cur_tab == TAB_MOTION) prompt_list[1] = {ui::kBack, "Back"}, prompts_n = 2;   // (Esc: to Controls)
    if (selr.cheat >= 0) {   // (a debug tool: Change when Left and Right do something, then what confirm does)
        prompts_n = 0;
        if (selr.cheat <= 1 || selr.cheat == 7 || cheat_chooses(selr.cheat) || age_shown(selr.cheat))
            prompt_list[prompts_n++] = {ui::kLeftRight, "Change"};
        if (const char* says = cheat_confirm(selr.cheat)) prompt_list[prompts_n++] = {ui::kConfirm, says};
        prompt_list[prompts_n++] = {ui::kTabs, "Category"}, prompt_list[prompts_n++] = {ui::kBack, "Back"};
    }
    const float bar_top = ui::prompt_bar_top(c);   // (the strip itself drawn last, over all: below)
    const bool after = restart_changed(selr);
    caption(c, hx0, hx1, nullptr, selr.help, hint, pending_line.empty() ? nullptr : pending_line.c_str(), help_tall, after);
    // (measured on the game's screen: a row's plate 1824 x 124, 170 apart, the first one's top 344 under the screen's top; the
    // value's near-black panel 804 x 108, its middle 500 right of the list's)
    const float rx = ui::at(c, kListX, 0).x, half = ui::lu(c, 912), rx0 = rx - half, rx1 = rx + half;
    const float rh = ui::lu(c, 124), pitch = ui::lu(c, 170), top = ui::at(c, 0, 736).y, bottom = ui::at(c, 0, -790).y;
    const float vx = rx + ui::lu(c, 500), vhw = ui::lu(c, 402), vhh = ui::lu(c, 54);
    const float clip0 = hh - ui::lu(c, 14), clip1 = bar_top + ui::lu(c, 50);
    if (cur_tab == TAB_DEBUG) {   // (the Debug tab: its own layout of the same rows)
        marquee_max = 0;
        draw_debug_rows(c, cx0, cx1, top, bottom, sel);
        ui::prompt_bar(c, prompt_list, prompts_n);
        return;
    }
    int shown = 0;
    const float first = scroll_rows(cur_tab, count, sel, top, bottom, pitch, &shown);
    scroll_bar(c, ui::at(c, kListX - 1102, 0).x, ui::at(c, 0, 100).y, ui::at(c, 0, -212).y, count, shown, first);   // (PaScrollBar: 56 under the middle)
    marquee_max = 0;   // (set again below while the selected row's value is too long)
    c.d->PushClipRect(ImVec2(cx0, clip0), ImVec2(cx1, clip1), true);
    for (int i = 0; i < count; i++) {
        const float y = top + (i - first) * pitch, my = y + rh * 0.5f;
        if (y + rh < clip0 || y > clip1) continue;
        const Row& r = rows[tab_row_to_global(cur_tab, i)];
        if (r.heading) {   // (a section: the game's separator, its name small over the line's start)
            ui::separator(c, rx0 + ui::lu(c, 40), rx1 - ui::lu(c, 40), my);
            continue;
        }
        const bool selected = i == sel;
        name_cell(c, ImVec2(rx0, y), ImVec2(rx1, y + rh), r.name, selected, restart_changed(r));
        const ImVec2 va(vx - vhw, my - vhh), vb(vx + vhw, my + vhh);
        float t0 = 0, t1 = 0;
        if (!r.value) {   // (a page, or Restart and Quit: Confirm until the second press)
            const bool ends = &r == &rows[kRestartRow] || &r == &rows[kQuitRow];
            if (pending == &r) choice_cell(c, va, vb, "Press again", selected, false, false);
            else choice_cell(c, va, vb, ends ? r.name : "Open", selected, false, true, false, true);
        } else {
            const bool fixed = row_fixed(r.value);
            if (is_slider(r)) {
                slider_cell(c, va, vb, r.text(*r.value), menu_hit::slider_permille(*r.value, r.lo, r.hi), selected, &t0, &t1);
                t0 = t1 = 0;   // (no track to drag: the arrows step it)
            } else if (r.text == tx_onoff && !fixed) {   // (a toggle as the game's: On at the left, Off at the right)
                halves_cell(c, va, vb, "On", "Off", *r.value == 0, selected, fixed);
            } else {
                const bool output_row = r.value == &opts.output_res;
                const bool left = !fixed && (!output_row || step_output_choice(*r.value, -1) != *r.value) && (!stops_at_ends(r) || *r.value > r.lo);
                const bool right = !fixed && (!output_row || step_output_choice(*r.value, 1) != *r.value) && (!stops_at_ends(r) || *r.value < r.hi);
                choice_cell(c, va, vb, r.text(*r.value), selected, left, right, fixed);
            }
        }
        hits.add(rx0, std::max(y, clip0), rx1, std::min(y + rh, clip1), menu_hit::kRow, i, va.x, vb.x, t0, t1);
    }
    c.d->PopClipRect();
    if (cur_tab == TAB_SYSTEM) {   // (which build, so a report can say it without opening the log)
        const std::string ver = std::string("Triforce ") + HOOKSHOT_VERSION;
        ui::text_mid(c, c.f.body, ui::lu(c, 36), ui::at(c, -1840, 0).x, ui::at(c, 0, -1030).y, ui::rgba(255, 251, 238, 120), ver.c_str());
    }
    ui::prompt_bar(c, prompt_list, prompts_n);   // (the guides, on top of the caption and the rows)
}

void draw_controls(ui::Ctx& c) {
    // (the page's own rows first, kTop of them (the layout preset, the button prompts), then a row per control and Reset to
    // defaults: one list, csel its selection)
    const float s = c.s, hh = title_banner(c, "Controls", nullptr);
    float cx0, cx1, hx0, hx1;
    bool narrow;
    columns(c, &cx0, &cx1, &hx0, &hx1, &narrow);
    column(c, cx0, cx1, hh);
    const int n = kTop + kControls + 1, sel_i = csel - kTop;   // (sel_i: the control's, kControls Reset; negative: a page row)
    const ui::Prompt idle[] = {{ui::kUpDown, "Choose"}, {ui::kLeftRight, "Column"}, {ui::kConfirm, "Bind"}, {ui::kKey, "Clear", "Del"},
                               {ui::kBack, "Back"}};
    const ui::Prompt option[] = {{ui::kUpDown, "Choose"}, {ui::kLeftRight, "Change"}, {ui::kBack, "Back"}};
    const ui::Prompt wait[] = {{ui::kBack, "Cancel"}};
    const float bar_top = ui::prompt_bar_top(c);   // (the strip itself drawn last, over all: below)
    const Row* page_row = sel_i < 0 ? &rows[tab_row_to_global(TAB_PAGE, csel)] : nullptr;
    const char* title = page_row ? page_row->name : sel_i < kControls ? controls[sel_i].name : "Reset to defaults";
    const char* help = listening ? (listening == 1 ? "Press the gamepad button to use. Esc cancels." : "Press the key to use. Esc cancels.")
                       : page_row ? page_row->help
                                  : "Which gamepad button and which keys press each of the Switch's buttons. Enter binds the one "
                                    "selected; Delete clears it. The game's button icons follow after a restart.";
    const char* hint = page_row ? (page_row->hint ? page_row->hint(*page_row->value, -1) : nullptr)
                                : "Gamepad: the D-pad moves, A binds, B goes back.";
    const float bottom = bar_top - 6 * s;   // (the rows end above the guides)
    caption(c, hx0, hx1, title, help, hint, nullptr, 0, page_row && restart_changed(*page_row));
    // (the options' rows, a little lower: 24 controls. A plate across the row, its name at the left, the gamepad's input and the
    // keys on value plates at the right)
    const float rxm = ui::at(c, kListX, 0).x, rx0 = rxm - ui::lu(c, 912), rx1 = rxm + ui::lu(c, 912);
    const float pad_x = rx0 + (rx1 - rx0) * 0.34f, key_x = rx0 + (rx1 - rx0) * 0.67f;
    float top = hh + 12 * s;
    ui::text_mid_centre(c, c.f.body, ui::lu(c, 40), (pad_x + key_x) * 0.5f, top + 9 * s, ui::kSoft, "Gamepad");
    ui::text_mid_centre(c, c.f.body, ui::lu(c, 40), (key_x + rx1) * 0.5f - ui::lu(c, 8), top + 9 * s, ui::kSoft, "Keyboard");
    top += 24 * s;
    const float rh = ui::lu(c, 112), pitch = ui::lu(c, 140), vin = ui::lu(c, 8);
    int shown = 0;
    const float first = scroll_rows(kControlsSlot, n, csel, top, bottom, pitch, &shown);
    scroll_bar(c, cx0 + 20 * s, top, top + shown * pitch - (pitch - rh), n, shown, first);
    c.d->PushClipRect(ImVec2(cx0, top - 8 * s), ImVec2(cx1, bottom + 8 * s));
    for (int j = 0; j < n; j++) {
        const float y = top + (j - first) * pitch;
        if (y + rh < top - 8 * s || y > bottom + 8 * s) continue;
        const bool sel = j == csel;
        const int i = j - kTop;
        const float y0 = std::max(y, top - 8 * s), y1 = std::min(y + rh, bottom + 8 * s);
        if (i < 0) {   // (one of the page's own rows: a name and a choice, as on the tabs)
            const Row& r = rows[tab_row_to_global(TAB_PAGE, j)];
            // (their names wider than a control's, as wide as the tabs' rows': "Button prompts" whole at any size)
            const float opt_x = rx0 + (rx1 - rx0) * 0.5f;
            name_cell(c, ImVec2(rx0, y), ImVec2(rx1, y + rh), r.name, sel, restart_changed(r), 17);
            choice_cell(c, ImVec2(opt_x, y + vin), ImVec2(rx1 - vin, y + rh - vin), r.text(*r.value), sel, *r.value > r.lo, *r.value < r.hi);
            hits.add(rx0, y0, rx1, y1, menu_hit::kCtlOption, j, opt_x, rx1);
            continue;
        }
        if (i == kControls) {
            ui::button(c, ImVec2(rx0, y + ui::lu(c, 10)), ImVec2(pad_x + ui::lu(c, 120), y + rh + ui::lu(c, 10)), "Reset to defaults", sel);
            hits.add(rx0, y0, pad_x + ui::lu(c, 120), y1 + ui::lu(c, 10), menu_hit::kCtlRow, i);
            continue;
        }
        const Control& ctl = controls[i];
        name_cell(c, ImVec2(rx0, y), ImVec2(rx1, y + rh), ctl.name, sel, false, 17);
        hits.add(rx0, y0, pad_x, y1, menu_hit::kCtlRow, i);
        for (int col = 0; col < 2; col++) {   // (two value cells, a gap between them, each drawn as the tabs' values are)
            const float a = col ? key_x + 3 * s : pad_x, b = col ? rx1 - vin : key_x - 3 * s;
            std::string v = col == 0 ? (keys_only(i) ? "-" : pad_name(host->binding(HOS_BIND_PAD, ctl.button, 0))) : keys_text(ctl.button);
            const bool here = sel && col == ccol;
            const ImVec2 va(a, y + vin), vb(b, y + rh - vin);
            if (here && listening) {   // (waiting for the new input: the light plate, the cursor round it)
                ui::value_plate(c, va, vb);
                ui::text_mid_centre(c, c.f.body, ui::lu(c, 50), (a + b) * 0.5f, y + rh * 0.5f - ui::lu(c, 2), ui::kInk,
                                    col == 0 ? "Press a button..." : "Press a key...");
                ui::cursor(c, va - ImVec2(ui::lu(c, 2.5f), ui::lu(c, 3)), vb + ImVec2(ui::lu(c, 2.5f), ui::lu(c, 3)));
            } else {
                choice_cell(c, va, vb, v, here, false, false, col == 0 && keys_only(i));
            }
            if (!(col == 0 && keys_only(i))) hits.add(a, y0, b, y1, menu_hit::kCtlCell, i, 0, 0, 0, 0, col);
        }
    }
    c.d->PopClipRect();
    (void)(listening ? ui::prompt_bar(c, wait, 1) : sel_i < 0 ? ui::prompt_bar(c, option, 3) : ui::prompt_bar(c, idle, 5));   // (the prompts, on top of the help and the rows)
}

/// A small square page arrow at a (its size side): dir 0 back, 1 on; dim when it cannot go that way
void page_arrow(ui::Ctx& c, ImVec2 a, float side, int dir, bool can) {
    const float s = c.s;
    const ImVec2 b = a + ImVec2(side, side);
    c.d->AddRectFilled(a, b, can ? ui::kCellName : ui::alpha(ui::kCellName, 0.4f), 3 * s);
    c.d->AddRect(a, b, can ? ui::kEdge : ui::alpha(ui::kEdge, 0.4f), 3 * s, std::max(1.0f, s));
    ui::triangle(c, (a + b) * 0.5f, 10 * s, dir, can ? ui::kInkBright : ui::kDim);
}

/// A tag at the right end of a card (its right edge x, its middle y): the text on an accent plate; its width
float card_tag(ui::Ctx& c, float x, float y, const char* tag, bool draw = true) {
    const float s = c.s, tw = ui::size_of(c.f.strong, 13 * s, tag).x + 14 * s;
    if (draw) {
        const ImVec2 g(x - tw, y - 10 * s);
        c.d->AddRectFilled(g, g + ImVec2(tw, 20 * s), ui::kAccent, 2 * s);
        ui::text_mid(c, c.f.strong, 13 * s, g.x + 7 * s, y, ui::kGoldInk, tag);
    }
    return tw;
}

void draw_inventory(ui::Ctx& c) {
    const float s = c.s;
    const int begin = inventory_begin(), end = inventory_end();
    bool loaded = false;   // (a save's counts reported: -1 for all until the game side has a save to read)
    for (int i = 0; i < kItems && !loaded; ++i) loaded = item_counts[i].load(std::memory_order_relaxed) >= 0;
    char count_text[48];
    snprintf(count_text, sizeof count_text, "%d items", end - begin);
    const float hh = title_banner(c, "Inventory", count_text);
    const int have = item_counts[item_choice].load(std::memory_order_relaxed);
    const char* edit = item_kinds[item_choice] ? (inventory_amount ? "Done" : "Edit amount") : (have ? "Relock" : "Unlock");
    ui::Prompt prompts[5] = {{ui::kUpDown, "Move"}, {ui::kLeftRight, inventory_amount ? "Amount" : "Move"}, {ui::kTabs, "Section"}};
    int prompts_n = 3;
    if (have >= 0) prompts[prompts_n++] = {ui::kConfirm, edit};
    prompts[prompts_n++] = {ui::kBack, inventory_amount ? "Done" : "Back"};
    float cx0, cx1, hx0, hx1;
    bool narrow;
    columns(c, &cx0, &cx1, &hx0, &hx1, &narrow);
    column(c, cx0, cx1, hh);
    const float bar = ui::prompt_bar_top(c);   // (the strip itself drawn last, over all: below)
    const float left = cx0 + 24 * s, right = cx1 - 24 * s;
    const float gap = 10 * s;
    // the sections, as tabs over the grid (Q and E, or a click)
    // (on one line: their padding, then their text, made smaller as far as needed for the column's width)
    float tpx = 16 * s, tpad = 26 * s, tgap = 8 * s;
    auto tabs_w = [&] {
        float w = 0;
        for (int i = 0; i <= kItemSections; ++i) w += ui::size_of(c.f.strong, tpx, i ? item_sections.s[i - 1].name : "All").x + tpad + tgap;
        return w - tgap;
    };
    if (tabs_w() > right - left) tpad = 16 * s, tgap = 6 * s;
    if (const float w = tabs_w(); w > right - left) {
        const float text = w - (kItemSections + 1) * tpad - kItemSections * tgap;
        tpx = std::max(12 * s, tpx * (right - left - (w - text)) / text);
    }
    float x = left, y = hh + 18 * s;
    for (int i = 0; i <= kItemSections; ++i) {
        const char* label = i ? item_sections.s[i - 1].name : "All";
        const float w = ui::size_of(c.f.strong, tpx, label).x + tpad;
        if (x > left && x + w > right) x = left, y += 36 * s;
        const ImVec2 a(x, y), b(std::min(x + w, right), y + 30 * s);
        const bool active = i == inventory_filter;
        ui::value_plate(c, a, b, active);
        ui::text_mid_centre(c, c.f.strong, tpx, (a.x + b.x) * .5f, y + 15 * s, active ? ui::kValueOnInk : ui::kInk, label);
        hits.add(a.x, a.y, b.x, b.y, menu_hit::kRow, i);
        x += w + tgap;
    }
    float top = y + 44 * s;
    const float pitch = (loaded ? 62 : 46) * s;   // (the title screen: the names alone, no line under them)
    if (!loaded) {   // (the title screen: one note for the page, not "Load a save" on every card)
        ui::diamond(c, ImVec2(left + 5 * s, top - 2 * s), 4 * s, ui::kGoldHi);
        ui::text_mid(c, c.f.body, 16 * s, left + 18 * s, top - 2 * s, ui::kSoft, "Load a save to edit the inventory.");
        top += 30 * s;
    }
    const float strip = 58 * s, detail = bar - 14 * s - strip, grid_bottom = std::max(top, detail - 8 * s);
    const char* help = have < 0 ? "Load a save to edit this item." : item_kinds[item_choice] ?
        "Choose Edit amount, then use Left and Right to change the quantity." : "Unlock or relock this item.";
    caption(c, hx0, hx1, item_names[item_choice], help, nullptr, nullptr);
    // (as many columns as fit the longest name, at most 5; whole pages of cards, the range and the page under them)
    static float name_w = 0, name_s = 0;
    if (name_s != s) {
        name_s = s, name_w = 0;
        for (int i = 0; i < kItems; ++i) name_w = std::max(name_w, ui::size_of(c.f.strong, 15.5f * s, item_names[i]).x);
    }
    inventory_columns = std::clamp((int)((right - left + gap) / std::max(name_w + 26 * s + gap, 150 * s)), 1, 5);
    inventory_capacity = inventory_columns * std::max(1, (int)((grid_bottom - top + gap) / pitch));
    const int pages_n = std::max(1, (end - begin + inventory_capacity - 1) / inventory_capacity);
    inventory_page = std::clamp(inventory_page, 0, pages_n - 1);
    const int first = begin + inventory_page * inventory_capacity;
    const float width = (right - left + gap) / inventory_columns;
    c.d->PushClipRect(ImVec2(left - 12 * s, top - 12 * s), ImVec2(right + 12 * s, grid_bottom + 12 * s));
    for (int i = first; i < std::min(end, first + inventory_capacity); ++i) {
        const int cell = i - first;
        const ImVec2 a(left + (cell % inventory_columns) * width, top + (cell / inventory_columns) * pitch);
        const ImVec2 b(a.x + width - gap, a.y + pitch - gap);
        const int count = item_counts[i].load(std::memory_order_relaxed);
        const bool selected = i == item_choice, owned = count > 0, editing = selected && inventory_amount;
        ui::plate(c, a, b, owned);
        c.d->PushClipRect(a + ImVec2(8 * s, 0), b - ImVec2(8 * s, 0), true);
        ui::text_mid(c, c.f.strong, 15.5f * s, a.x + 13 * s, loaded ? a.y + 17 * s : (a.y + b.y) * 0.5f, count < 0 ? ui::kDim : ui::kInkBright, item_names[i]);
        char amount[40];
        if (count < 0) snprintf(amount, sizeof amount, "%s", loaded ? "Unavailable" : "");
        else if (item_kinds[i]) snprintf(amount, sizeof amount, "Quantity %d", count);
        else snprintf(amount, sizeof amount, "%s", count ? "Unlocked" : "Locked");
        const float ly = a.y + 37 * s;
        if (editing) {   // (the amount being changed: its arrows, gold)
            ui::triangle(c, ImVec2(a.x + 18 * s, ly), 8 * s, 0, ui::kGoldHi);
            ui::text_mid(c, c.f.strong, 15 * s, a.x + 28 * s, ly, ui::kGoldHi, amount);
            ui::triangle(c, ImVec2(a.x + 38 * s + ui::size_of(c.f.strong, 15 * s, amount).x, ly), 8 * s, 1, ui::kGoldHi);
        } else {
            ui::text_mid(c, c.f.body, 15 * s, a.x + 13 * s, ly, owned ? ui::kAccent : ui::kDim, amount);
        }
        c.d->PopClipRect();
        if (selected) ui::glow(c, a, b);
        if (a.y < grid_bottom) hits.add(a.x, a.y, b.x, std::min(b.y, grid_bottom), menu_hit::kListLine, i);
    }
    c.d->PopClipRect();
    if (detail >= top) {   // (the page: its arrows, Page n of m and the range; the selected item's action at the right)
        c.d->AddLine(ImVec2(left, detail), ImVec2(right, detail), ui::alpha(ui::kEdge, 0.8f));
        const int pages = (end - begin + inventory_capacity - 1) / inventory_capacity, at = (first - begin) / inventory_capacity;
        const float my = detail + strip * 0.5f + 2 * s, side = 32 * s;
        float px0 = left;
        if (pages > 1) {
            page_arrow(c, ImVec2(px0, my - side * 0.5f), side, 0, at > 0);
            hits.add(px0, my - side * 0.5f, px0 + side, my + side * 0.5f, menu_hit::kButton, 2);
            char page_text[32];
            snprintf(page_text, sizeof page_text, "Page %d of %d", at + 1, pages);
            const float tw = ui::size_of(c.f.strong, 17 * s, page_text).x;
            ui::text_mid(c, c.f.strong, 17 * s, px0 + side + 12 * s, my, ui::kInkBright, page_text);
            const float nx = px0 + side + 24 * s + tw;
            page_arrow(c, ImVec2(nx, my - side * 0.5f), side, 1, at + 1 < pages);
            hits.add(nx, my - side * 0.5f, nx + side, my + side * 0.5f, menu_hit::kButton, 3);
            px0 = nx + side + 18 * s;
        }
        char range[64];
        snprintf(range, sizeof range, "%dâ€“%d of %d", first - begin + 1, std::min(end, first + inventory_capacity) - begin, end - begin);
        ui::text_mid(c, c.f.body, 15 * s, px0, my, ui::kSoft, range);
        if (have >= 0) {
            const float bw = ui::size_of(c.f.strong, 18 * s, "Edit amount").x + 36 * s;
            const ImVec2 a(right - bw, my - 17 * s), b(right, my + 17 * s);
            ui::button(c, a, b, edit, false);
            hits.add(a.x, a.y, b.x, b.y, menu_hit::kButton, 0);
            if (inventory_amount) {
                char amount[32];
                snprintf(amount, sizeof amount, "%d", have);
                const ImVec2 av(a.x - 118 * s, a.y), bv(a.x - 10 * s, b.y);
                choice_cell(c, av, bv, amount, true, true, true);
                hits.add(av.x, av.y, bv.x, bv.y, menu_hit::kButton, 1, av.x, bv.x);
            }
        }
    }
    ui::prompt_bar(c, prompts, prompts_n);   // (the prompts, on top of the help and the cards)
}

void draw_teleport(ui::Ctx& c) {
    const float s = c.s;
    char count[32];
    snprintf(count, sizeof count, "%d of %d", teleport_choice + 1, kTeleports);
    const float hh = title_banner(c, "Teleport", count);
    float cx0, cx1, hx0, hx1;
    bool narrow;
    columns(c, &cx0, &cx1, &hx0, &hx1, &narrow);
    column(c, cx0, cx1, hh);
    const ui::Prompt prompts[] = {{ui::kUpDown, "Move"}, {ui::kLeftRight, "Move"}, {ui::kTabs, "Section"},
        {ui::kConfirm, "Travel"}, {ui::kBack, "Back"}};
    const float bar = ui::prompt_bar_top(c);   // (the strip itself drawn last, over all: below)
    const float left = cx0 + 24 * s, right = cx1 - 24 * s, top = hh + 16 * s;
    const float strip = 58 * s, detail = bar - 14 * s - strip;
    const float bottom = std::max(top, detail - 6 * s), gap = 10 * s, pitch = 48 * s, head = 40 * s;
    const char* const note = teleport_notes[teleport_choice];
    caption(c, hx0, hx1, teleport_names[teleport_choice],
            "Select a destination, then Travel (or Enter) to go to its map entrance. Link keeps his current age.", note, nullptr);
    // (as many columns as fit the longest name and its tag, at most 4; each section under its heading, the cards scrolled to
    // keep the selected one in view)
    static float card_w = 0, card_s = 0;
    if (card_s != s) {
        card_s = s, card_w = 0;
        for (int i = 0; i < kTeleports; ++i)
            card_w = std::max(card_w, ui::size_of(c.f.body, 17 * s, teleport_names[i]).x +
                                          (teleport_notes[i] ? card_tag(c, 0, 0, teleport_notes[i], false) + 12 * s : 0));
    }
    teleport_columns = std::clamp((int)((right - left + gap) / (card_w + 36 * s + gap)), 1, 4);
    const int cols = teleport_columns;
    const float width = (right - left + gap) / cols;
    // (the y of each section's heading and first row, from the top of the list)
    float section_y[kTeleportSections + 1];
    float total = 0;
    for (int k = 0; k < kTeleportSections; ++k) {
        section_y[k] = total;
        const int n = teleport_section_end(k) - teleport_sections[k].first;
        total += head + (n + cols - 1) / cols * pitch + (k + 1 < kTeleportSections ? 10 * s : 0);
    }
    section_y[kTeleportSections] = total;
    auto card_y = [&](int i) {
        const int k = teleport_section_of(i);
        return section_y[k] + head + (i - teleport_sections[k].first) / cols * pitch;
    };
    const float view = bottom - top;
    teleport_page_rows = std::max(1, (int)(view / pitch) - 1);
    float& at = teleport_scroll_px;
    float& want = teleport_want_px;
    if (teleport_wheel) want -= teleport_wheel * pitch, teleport_wheel = 0;   // (the wheel: the cards move, the choice stays)
    if (total <= view) want = 0;
    else if (!teleport_follow) want = std::clamp(want, 0.0f, total - view);
    else {
        const int k = teleport_section_of(teleport_choice);
        const bool first_row = teleport_choice - teleport_sections[k].first < cols;
        const float y0 = first_row ? section_y[k] : card_y(teleport_choice) - pitch * 0.5f, y1 = card_y(teleport_choice) + pitch * 1.5f;
        if (y0 < want) want = y0;
        if (y1 > want + view) want = y1 - view;
        want = std::clamp(want, 0.0f, total - view);
    }
    at += (want - at) * ease_step;
    if (std::fabs(want - at) < 0.5f) at = want;
    if (total > view) scroll_bar(c, cx0 + 12 * s, top, bottom, (int)total, (int)view, at);
    c.d->PushClipRect(ImVec2(left - 12 * s, top), ImVec2(right + 12 * s, bottom), true);
    for (int k = 0; k < kTeleportSections; ++k) {
        const float hy = top + section_y[k] - at + head * 0.5f;
        if (hy > bottom + head || hy < top - head) continue;
        ui::text_mid(c, c.f.heading, 20 * s, left, hy, ui::kInkBright, teleport_sections[k].name);
        const float tw = ui::size_of(c.f.heading, 20 * s, teleport_sections[k].name).x;
        char n_text[16];
        snprintf(n_text, sizeof n_text, "%d", teleport_section_end(k) - teleport_sections[k].first);
        const float nw = ui::size_of(c.f.body, 15 * s, n_text).x;
        ui::text_mid(c, c.f.body, 15 * s, right - nw, hy, ui::kSoft, n_text);
        c.d->AddLine(ImVec2(left + tw + 14 * s, hy + s), ImVec2(right - nw - 12 * s, hy + s), ui::alpha(ui::kAccent, 0.45f),
                     std::max(1.0f, s));
    }
    for (int i = 0; i < kTeleports; ++i) {
        const int k = teleport_section_of(i), cell = i - teleport_sections[k].first;
        const ImVec2 a(left + cell % cols * width, top + card_y(i) - at);
        const ImVec2 b(a.x + width - gap, a.y + pitch - 8 * s);
        if (b.y < top || a.y > bottom) continue;
        const bool selected = i == teleport_choice;
        ui::plate(c, a, b, selected);
        float name_end = b.x - 10 * s;
        if (teleport_notes[i]) name_end -= card_tag(c, b.x - 8 * s, (a.y + b.y) * 0.5f, teleport_notes[i]) + 8 * s;
        c.d->PushClipRect(a, ImVec2(name_end, b.y), true);
        ui::text_mid(c, c.f.body, 17 * s, a.x + 16 * s, (a.y + b.y) * 0.5f, ui::kInk, teleport_names[i]);
        c.d->PopClipRect();
        if (selected) ui::glow(c, a, b);
        hits.add(a.x, std::max(a.y, top), b.x, std::min(b.y, bottom), menu_hit::kListLine, i);
    }
    c.d->PopClipRect();
    if (detail >= top) {   // (the selection, its section and note, and Travel: what Enter does)
        c.d->AddLine(ImVec2(left, detail), ImVec2(right, detail), ui::alpha(ui::kEdge, 0.8f));
        const float my = detail + strip * 0.5f + 2 * s;
        const float bw = ui::size_of(c.f.strong, 20 * s, "Travel").x + 56 * s;
        const ImVec2 a(right - bw, my - 18 * s), b(right, my + 18 * s);
        c.d->PushClipRect(ImVec2(left, detail), ImVec2(a.x - 12 * s, detail + strip), true);
        ui::text_mid(c, c.f.strong, 18 * s, left, my - 10 * s, ui::kInkBright, teleport_names[teleport_choice]);
        char where[64];
        snprintf(where, sizeof where, note ? "%s Â· %s" : "%s", teleport_sections[teleport_section_of(teleport_choice)].name, note);
        ui::text_mid(c, c.f.body, 15 * s, left, my + 12 * s, ui::kAccent, where);
        c.d->PopClipRect();
        ui::button(c, a, b, "Travel", false);
        hits.add(a.x, a.y, b.x, b.y, menu_hit::kButton, 0);
    }
    ui::prompt_bar(c, prompts, 5);   // (the prompts, on top of the help and the cards)
}

void draw_ask(ui::Ctx& c) {
    const float s = c.s;
    c.d->AddRectFilled(ImVec2(0, 0), ImVec2(c.w, c.h), ui::rgba(8, 5, 2, 110));   // (the dialog over the page: darker still)
    ui::banner(c, 0, "Restart Needed");
    const std::string ask = "Restart to apply " + restart_list(" and ") + "?";
    const float wrap = ui::lu(c, 1440), px = ui::lu(c, 54 * ui::kType);
    const float th = ui::para(c, c.f.body, px, ImVec2(), 0, ask.c_str(), wrap, false);
    float y = ui::at(c, 0, 250).y - th * 0.5f;
    {   // (each line centred, as the game's dialogs set their text)
        const char* t = ask.c_str();
        const char* end = t + ask.size();
        while (t < end) {
            const char* cut = c.f.body->CalcWordWrapPosition(px, t, end, wrap);
            if (cut == t) cut = t + 1;
            const float lw = c.f.body->CalcTextSizeA(px, FLT_MAX, 0, t, cut).x;
            c.d->AddText(c.f.body, px, ImVec2(c.w * 0.5f - lw * 0.5f, y), ui::kInk, t, cut);
            y += px * 1.3f;
            t = cut;
            while (t < end && (*t == ' ' || *t == '\n')) t++;
        }
    }
    ui::text_mid_centre(c, c.f.body, ui::lu(c, 52), c.w * 0.5f, y + 16 * s, ui::kSoft, "Progress since your last save is lost.");
    // (SystemWindow_00's buttons: PaCommonBtnThin's plate, 684 x 156 units, 188 apart; the upper one acts, the lower one backs out)
    const float bw = ui::lu(c, 336), bh = ui::lu(c, 72);
    const char* labels[2] = {"Later", "Restart Now"};
    for (int i = 0; i < 2; i++) {
        const float my = ui::at(c, 0, i ? -330.0f : -518.0f).y;
        const ImVec2 a(c.w * 0.5f - bw, my - bh), b(c.w * 0.5f + bw, my + bh);
        ui::button(c, a, b, labels[i], i == ask_sel);
        hits.add(a.x, a.y, b.x, b.y, menu_hit::kButton, i);
    }
    const ui::Prompt prompt_list[] = {{ui::kConfirm, "Confirm"}, {ui::kBack, "Later"}};
    ui::prompt_bar(c, prompt_list, 2);
}

void draw_menu(ui::Ctx& c);
/// The menu's text sizes at this window's unit (and, with Menu size Auto, at the other size Auto can pick), baked ahead by
/// vk_imgui.cpp a few a frame: the first open then draws with no glyphs to make (it took 2.6 ms at 1080p)
float prebaked[3] = {0, 0, 0};   // (the units and the width prebake last did)
/// The menu's units now (with Auto, both it can pick) for a window w x h: whether prebake has not done them yet
bool prebake_needed(uint32_t w, uint32_t h) {
    float m = 0;
    const float u0 = ui::unit((float)h, opts.menu_size, opts.fullscreen != 0, auto_pad, &m);
    const float u1 = opts.menu_size == 0 ? ui::unit((float)h, 0, opts.fullscreen != 0, !auto_pad, &m) : u0;
    return u0 != prebaked[0] || u1 != prebaked[1] || (float)w != prebaked[2];
}
void prebake(const ui::Ctx& c) {
    float units[2] = {c.s, c.s}, margins[2] = {c.safe, c.safe};
    if (opts.menu_size == 0) units[1] = ui::unit(c.h, 0, opts.fullscreen != 0, !auto_pad, &margins[1]);
    if (units[0] == prebaked[0] && units[1] == prebaked[1] && c.w == prebaked[2]) return;
    prebaked[0] = units[0], prebaked[1] = units[1], prebaked[2] = c.w;
    static const float points[] = {12, 12.5f, 13, 13.6f, 14, 14.28f, 14.5f, 15, 15.5f, 16, 16.5f, 17, 18, 18.5f, 19, 20, 21, 23, 24};
    constexpr int n = sizeof points / sizeof points[0];
    float px[2 * n];
    for (int u = 0; u < 2; u++)
        for (int i = 0; i < n; i++) px[u * n + i] = points[i] * units[u];
    ui::prebake(px, units[1] == units[0] ? n : 2 * n);
    if (open_now.load(std::memory_order_relaxed)) return;
    // (and the page itself drawn once into a list of its own, thrown away: what the first open would measure and bake (the
    // help's height over every row, the exact sizes) done now, the menu's state as it was)
    const menu_hit::Map saved_hits = hits;
    const int saved_marquee = marquee_max;
    const int saved_tele[2] = {teleport_columns, teleport_page_rows}, saved_inv[3] = {inventory_columns, inventory_capacity, inventory_page};
    const float saved_tele_px[2] = {teleport_scroll_px, teleport_want_px};
    const float saved_ease = ease_step;
    float saved_scroll[sizeof scroll_at / sizeof *scroll_at], saved_want[sizeof scroll_at / sizeof *scroll_at];
    int saved_to[sizeof scroll_at / sizeof *scroll_at];   // (the scratch drawing's scroll, given back: the next is the menu's)
    memcpy(saved_scroll, scroll_at, sizeof scroll_at);
    memcpy(saved_want, scroll_want, sizeof scroll_want);
    memcpy(saved_to, scrolled_to, sizeof scrolled_to);
    ImDrawList scratch(ImGui::GetDrawListSharedData());
    scratch._ResetForNewFrame();
    scratch.PushTexture(ImGui::GetIO().Fonts->TexRef);
    scratch.PushClipRectFullScreen();
    ease_step = 0;
    for (int u = 0; u < (units[1] == units[0] ? 1 : 2); u++) {
        ui::Ctx dry = c;
        dry.d = &scratch;
        dry.s = units[u], dry.safe = margins[u];
        draw_menu(dry);
    }
    hits = saved_hits, marquee_max = saved_marquee, ease_step = saved_ease;
    teleport_columns = saved_tele[0], teleport_page_rows = saved_tele[1], teleport_scroll_px = saved_tele_px[0], teleport_want_px = saved_tele_px[1];
    inventory_columns = saved_inv[0], inventory_capacity = saved_inv[1], inventory_page = saved_inv[2];
    memcpy(scroll_at, saved_scroll, sizeof scroll_at);
    memcpy(scroll_want, saved_want, sizeof scroll_want);
    memcpy(scrolled_to, saved_to, sizeof scrolled_to);
}

float menu_ox = 0;   // (the menu's column's left edge in the window: more than 0 only on a window wider than 16:9)
/// The menu: the game dimmed across the whole window, the pages in a column as wide as a 16:9 screen of this height, in the
/// middle (a window of 16:9 or narrower: the whole of it). The pages draw from 0 as on 16:9; their vertices and clip
/// rectangles are moved over after, and menu_tick moves the mouse back by menu_ox before it tests the hits
void draw_menu(ui::Ctx& c) {
    if (!embed_open) c.d->AddRectFilled(ImVec2(0, 0), ImVec2(c.w, c.h), ui::rgba(8, 5, 2, ui::kDimAlpha));   // (the game behind, dimmed)
    ui::Ctx k = c;
    k.over_game = embed_open;   // (inside the game's own Options screen: its background stays, its list is under our plates)
    k.w = std::min(c.w, std::floor(c.h * 16.0f / 9.0f + 0.5f));
    menu_ox = std::floor((c.w - k.w) * 0.5f);
    const int vtx0 = c.d->VtxBuffer.Size, cmd0 = c.d->CmdBuffer.Size;
    hits.clear(0, 0, k.w, k.h);
    if (page != 5) {   // (the System screens' dark column behind the list: SystemBg_05, under the header)
        float cx0, cx1, hx0, hx1;
        bool narrow;
        columns(k, &cx0, &cx1, &hx0, &hx1, &narrow);
        ui::column_bg(k, cx0, cx1);
    }
    if (page == 5) draw_ask(k);
    else if (page == 1) draw_controls(k);
    else if (page == 3) draw_teleport(k);
    else if (page == 4) draw_inventory(k);
    else draw_options(k);
    if (menu_ox <= 0) return;
    for (int i = vtx0; i < c.d->VtxBuffer.Size; i++) c.d->VtxBuffer[i].pos.x += menu_ox;
    // (a clip rectangle of the column's: moved with it; the whole window's (the dim's, and the draw list's own) stays, as it
    // holds the column wherever it is)
    for (int i = std::max(0, cmd0 - 1); i < c.d->CmdBuffer.Size; i++) {
        ImVec4& r = c.d->CmdBuffer[i].ClipRect;
        if (r.x <= 0 && r.z >= c.w) continue;
        r.x += menu_ox, r.z += menu_ox;
    }
    // (the draw list's current header, for whatever is drawn next: the window's again after the pages' pops)
}

/// The FPS counter, as a counter of the game's HUD: the rate in the HUD's own digits (NinDev-Number, the rupee counter's
/// font), white with the HUD's soft dark edge, "FPS" small beside it in the menus' font; with frame generation the game's
/// own rate after a slash, smaller, as the HUD writes an amount of its most ("12 / 99"). No plate: the HUD has none.
/// Where: at the top, its right end 500 units left of the screen's right edge, its middle 60 units down. Measured on the
/// game's own screens (1920 x 1080), that strip is free in play and in the menus alike: left of the item box, its page dots
/// and R key (from 1725 px) and above the fishing slots (from 53 px down); in the pause menu between the tabs' R icon
/// (to 1470 px) and the rupee counter (from 1715 px); right of the hearts and the mini-game timer. Under the header, at
/// the right, while our own options are open (their tabs' R icon is in that strip)
void draw_fps(ui::Ctx& c, bool under_banner) {
    if (!fps_text[0]) return;
    ImFont* digits = c.f.number ? c.f.number : c.f.strong;
    const float big = ui::lu(c, 72), small = ui::lu(c, 46), label = ui::lu(c, 34), gap = ui::lu(c, 12);
    const char* game = fps_sub[0] ? fps_sub : nullptr;   // ("60": the game's own rate, with frame generation)
    const float nw = ui::size_of(digits, big, fps_text).x, lw = ui::size_of(c.f.strong, label, "FPS").x;
    const float sw = game ? ui::size_of(digits, small, "/").x + ui::lu(c, 8) + ui::size_of(digits, small, game).x + gap : 0;
    const float total = nw + gap + sw + lw;
    const bool below = under_banner && page != 5;   // (our options open: under their header)
    // (a window wider than 16:9: the HUD keeps its 16:9 shape in the middle, and so does the counter)
    const float hud_w = std::min(c.w, std::floor(c.h * 16.0f / 9.0f + 0.5f)), hud_x1 = (c.w + hud_w) * 0.5f;
    const float x1 = below ? hud_x1 - ui::lu(c, 80) : hud_x1 - ui::lu(c, 500);
    const float my = below ? banner_h(c) + ui::lu(c, 50) : ui::lu(c, 60);
    auto edged = [&](ImFont* f, float px, float x, float y, const char* t, ImU32 col) {   // (the HUD's dark edge: the text under itself, soft)
        const float e = std::max(1.0f, px * 0.05f);
        for (int k = 0; k < 8; k++) {
            const float ang = k * 0.785398f;
            c.d->AddText(f, px, ImVec2(x + std::cos(ang) * e, ui::mid_top(f, px, y) + std::sin(ang) * e + e * 0.6f), ui::rgba(20, 14, 6, 150), t);
        }
        c.d->AddText(f, px, ImVec2(x, ui::mid_top(f, px, y)), col, t);
        return ui::size_of(f, px, t).x;
    };
    float x = x1 - total;
    x += edged(digits, big, x, my, fps_text, ui::kInk) + gap;
    if (game) {
        x += edged(digits, small, x, my + ui::lu(c, 8), "/", ui::kSoft) + ui::lu(c, 8);
        x += edged(digits, small, x, my + ui::lu(c, 8), game, ui::kSoft) + gap;
    }
    edged(c.f.strong, label, x, my + ui::lu(c, 12), "FPS", ui::kAccent);
}

/// (Triforce) The game's own tab inside its Options screen: our header over the screen's own (the same picture), the tabs on
/// it with "Game" chosen; the rest is the game's. It comes in over the screen's opening and goes with its closing
void draw_game_tab(ui::Ctx& c) {
    ui::Ctx k = c;
    k.w = std::min(c.w, std::floor(c.h * 16.0f / 9.0f + 0.5f));
    const float ox = std::floor((c.w - k.w) * 0.5f);
    const int vtx0 = c.d->VtxBuffer.Size;
    ui::banner(k, 0);
    draw_tabs(k, -1, true, false);
    static const bool still = getenv("RX_VK_UI_STILL") != nullptr;
    const int64_t now = prof_now(), seen = game_options_ns.load(std::memory_order_relaxed);
    float f = 1;
    if (!still) {
        f = std::clamp((float)((now - embed_since - 120000000LL) / 180e6), 0.0f, 1.0f);              // (in: after the screen's first frames)
        f *= 1 - std::clamp((float)((now - seen - 60000000LL) / 150e6), 0.0f, 1.0f);                  // (out: once it no longer runs)
    }
    for (int i = vtx0; i < c.d->VtxBuffer.Size; i++) {
        ImDrawVert& v = c.d->VtxBuffer[i];
        v.pos.x += ox;
        if (f < 1) v.col = ui::alpha(v.col, f);
    }
}

/// (Triforce) A line at the foot of the screen for a moment, as a guide of the game's: the Debug tab unlocked or locked
void draw_notice(ui::Ctx& c) {
    const float my = ui::at(c, 0, -1018).y, right = ui::at(c, 1840, 0).x, px = ui::lu(c, 48);
    const float w = ui::size_of(c.f.body, px, notice_text).x;
    if (const ui::Art* t = ui::art("SystemGuideBase_00")) ui::image(c, t, ImVec2(right - w - ui::lu(c, 300), ui::prompt_bar_top(c)), ImVec2(c.w + ui::lu(c, 4), c.h), ui::rgba(0, 0, 0, 215), true);
    else c.d->AddRectFilled(ImVec2(right - w - ui::lu(c, 60), my - px), ImVec2(c.w, c.h), ui::kBar);
    ui::text_mid(c, c.f.body, px, right - w, my, ui::kInk, notice_text);
}

/// The start-up screen: the logo, the wordmark, what the start is doing, a bar, on a warm dark. A little below the middle; what
/// is under the logo at 0.86 of the logo's scale. startup_fade, 0 to 1, darkens it all to black as it ends
void draw_startup(ui::Ctx& c) {
    StartupView v;
    {
        std::lock_guard lk(startup_mu);
        v = startup;
    }
#if __has_include("vk_startup_draw.h")   // (the start-up screen's own drawing, when that file is in the tree; a fade of 2 is its last picture)
    draw_startup_screen(c, v, startup_fade.load(std::memory_order_relaxed));
    return;
#endif
    const float s = c.s, w = c.w, h = c.h, cx = w * 0.5f, k = s * 0.86f;   // (k: the scale of what is under the logo)
    c.d->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), ui::rgba(14, 10, 6));
    const float lh = 112 * s, top = h * 0.5f - 176 * s;
    for (int i = 40; i >= 1; i--) c.d->AddCircleFilled(ImVec2(cx, top + lh * 0.5f + 70 * s), h * 0.021f * i, ui::rgba(46, 33, 18, 4), 128);
    float aspect = 1;
    ImTextureData* logo = ui::logo(&aspect);
    if (logo) c.d->AddImage(logo->GetTexRef(), ImVec2(cx - lh * aspect * 0.5f, top), ImVec2(cx + lh * aspect * 0.5f, top + lh));
    float y = top + lh + 16 * s;
    ui::spaced(c, c.f.heading, 42 * k, ImVec2(cx, y), ui::rgba(246, 232, 204), "HOOKSHOT", 6 * k);
    ui::flourish(c, cx - 190 * k, cx + 190 * k, y + 66 * k, ui::kAccent);
    ui::text_centre(c, c.f.body, 14.5f * k, ImVec2(cx, y + 78 * k), ui::kDim, v.version.c_str());
    if (!v.title.empty()) {
        y += 120 * k;
        if (!v.step.empty()) {   // ("STEP 2 OF 2" as "Step 2 of 2")
            std::string step = v.step;
            for (size_t i = 1; i < step.size(); i++) step[i] = (char)tolower((unsigned char)step[i]);
            ui::text_centre(c, c.f.body, 15 * k, ImVec2(cx, y), ui::kAccent, step.c_str());
        }
        ui::text_centre(c, c.f.heading, 24 * k, ImVec2(cx, y + 20 * k), ui::kInk, v.title.c_str());
        const float x0 = cx - 270 * k, x1 = cx + 270 * k, by = y + 64 * k;
        float under = by;
        if (v.fraction >= 0) {   // (the bar as a value cell: a dark cell, gold inside, the percentage on it)
            const float bh = 26 * k, in = 4 * k, fx = x0 + in + (x1 - x0 - 2 * in) * (float)std::min(v.fraction, 1.0);
            c.d->AddRectFilled(ImVec2(x0, by), ImVec2(x1, by + bh), ui::kCellDark);
            c.d->AddRect(ImVec2(x0, by), ImVec2(x1, by + bh), ui::kEdge, 0, std::max(1.0f, s));
            if (fx > x0 + in) ui::gold(c, ImVec2(x0 + in, by + in), ImVec2(fx, by + bh - in));
            // (the percentage in the middle of the whole bar, drawn twice: dark ink where the gold is under it, light where the
            // empty bar is, each clipped to its part, so every letter keeps its contrast as the edge passes through it)
            const float split = std::max(fx, x0 + in);
            c.d->PushClipRect(ImVec2(x0, by), ImVec2(split, by + bh), true);
            ui::text_mid_centre(c, c.f.strong, 15 * k, cx, by + bh * 0.5f, ui::kGoldInk, v.pct.c_str());
            c.d->PopClipRect();
            c.d->PushClipRect(ImVec2(split, by), ImVec2(x1, by + bh), true);
            ui::text_mid_centre(c, c.f.strong, 15 * k, cx, by + bh * 0.5f, ui::kInk, v.pct.c_str());
            c.d->PopClipRect();
            under = by + bh;
        }
        const float px = 16.5f * k;
        ui::text(c, c.f.body, px, ImVec2(v.fraction >= 0 ? x0 : cx - ui::size_of(c.f.body, px, v.left.c_str()).x * 0.5f, under + 12 * k),
                 ui::kSoft, v.left.c_str());
        if (!v.right.empty())
            ui::text(c, c.f.body, px, ImVec2(x1 - ui::size_of(c.f.body, px, v.right.c_str()).x, under + 12 * k), ui::kSoft, v.right.c_str());
        if (!v.hint.empty()) ui::text_centre(c, c.f.body, 15.5f * k, ImVec2(cx, by + 92 * k), ui::kDim, v.hint.c_str());
    }
    if (const float f = startup_fade.load(std::memory_order_relaxed); f > 0)   // (the end: to black, the game's first frame next)
        c.d->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), ui::rgba(0, 0, 0, (int)(std::min(f, 1.0f) * 255 + 0.5f)));
}

} // namespace

void startup_fade_to(float f) { startup_fade.store(f, std::memory_order_relaxed); }

void startup_screen(const StartupView* view) {
    if (!view) {
        startup_on.store(false, std::memory_order_release);
        startup_fade.store(0, std::memory_order_relaxed);
        return;
    }
    StartupView v = *view;
    if (v.version.empty()) v.version = version_text(HOOKSHOT_VERSION);   // ("v2.0.5"; "v2.0.5 dev" between releases)
    {
        std::lock_guard lk(startup_mu);
        startup = v;
    }
    startup_on.store(true, std::memory_order_release);
}

int hdr_at_start = 0;
int renodx_at_start = 0, upscale_pre_at_start = 0;
bool renodx_chosen() {
    static const char* const e = getenv("RX_VK_RENODX");
    return e && *e ? atoi(e) != 0 : renodx_at_start != 0;
}

void menu_init() {
    static bool done;
    if (done) return;
    done = true;
    for (auto& count : item_counts) count.store(-1, std::memory_order_relaxed);
    bool has_options = false;
    const char* cheats = getenv("HOS_OOT_CHEATS");
    cheats_env = cheats && !strcmp(cheats, "1");
    if (cheats_env) {
        const char* hearts = getenv("HOS_OOT_PROTECT_HEARTS");
        protect_hearts.store(hearts && !strcmp(hearts, "1"));
    }
    if (FILE* f = fopen(ini_path().c_str(), "r")) {
        has_options = true;
        char line[80];
        bool has_resolution = false;
        bool v2 = false;
        int version = 0;
        int old_dynres = -1;   // (the row before Resolution: 0, the full scale, is its 100%; 1 is Auto)
        while (fgets(line, sizeof line, f)) {
            char* eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            for (Row& r : rows) {
                if (!r.value || strcmp(line, ini_key(r))) continue;
                const int v = atoi(eq + 1);
                *r.value = r.value == &opts.output_res ? output_choice(v) : std::min(std::max(v, r.lo), r.hi);
            }
            has_resolution |= !strcmp(line, "resolution");
            v2 |= !strcmp(line, "options_version");
            if (!strcmp(line, "options_version")) version = atoi(eq + 1);
            if (!strcmp(line, "dynamic_resolution")) old_dynres = atoi(eq + 1);
            if (!strcmp(line, "debug_tab")) debug_unlocked = atoi(eq + 1) != 0;
            if (!strcmp(line, "microphone_device")) {   // (hookshot: the microphone, by its name)
                mic_want = mic_key(eq + 1);
                while (!mic_want.empty() && (mic_want.back() == '\n' || mic_want.back() == '\r')) mic_want.pop_back();
            }
        }
        fclose(f);
        hdr_at_start = opts.hdr;   // (local, RenoDX port: vk_menu.h)
        renodx_at_start = opts.renodx, upscale_pre_at_start = opts.upscale_pre;
        // (the saved microphone by its name, not its place; the host told the row's choice. Off or the default: no list, no SDL
        // audio at the start)
        if (!mic_want.empty()) refresh_mics();
        else {
            if (opts.mic >= 2) opts.mic = 1;
            mic_tell_host();
        }
        if (!has_resolution && old_dynres >= 0) opts.resolution = old_dynres ? 0 : 1;
        opts.rumble = rumble_step(opts.rumble);   // (a hand-edited 60: 50, on the arrows' steps)
        // (Game speed is Real time by default now: Per frame played at half speed at 30 fps, the "slow motion" players
        // reported. An options file from before kept the old default whether chosen or not: Real time for it too)
        if (!v2) opts.realtime = 1;
        // (Descriptor reuse is on by default from options_version 4: a file from before wrote its 0 whether chosen or not, as
        // the off default it then was; fewer stutters and a better 1% low in the field, the flash checks clean, 2026-11-10)
        if (version < 4) opts.descriptor_reuse = 1;
    }
    // An opt-in profile for 4 GB cards: keep existing options authoritative, but make a first run usable on
    // low-VRAM hardware without requiring the menu to render first.
    if (!has_options && getenv("RX_VK_LOW_SPEC") && atoi(getenv("RX_VK_LOW_SPEC")) != 0) {
        apply_preset(1);
        opts.upscale = 0;
        opts.sharpness = 0;
    }
    if (opts.preset > 0) sync_preset_budgets(opts.preset);
    if (const char* e = getenv("RX_VK_FRAMEGEN_MULT")) opts.framegen = std::min(std::max(atoi(e), 2), 8);
    if (const char* e = getenv("RX_VK_PACE")) opts.pace = atoi(e) != 0;
    if (const char* e = getenv("RX_VK_UPSCALE"))
        for (Row& r : rows)
            if (r.value == &opts.upscale) *r.value = std::min(std::max(atoi(e), r.lo), r.hi);
    if (const char* e = getenv("HOS_REALTIME_STEP")) opts.realtime = atoi(e) != 0;
    if (const char* e = getenv("HOS_DYNRES")) opts.resolution = atoi(e) != 0 ? 0 : 1;
    if (const char* e = getenv("HOS_OUTPUT_RES")) opts.output_res = output_choice(atoi(e));
    restart_init();   // (what this run started with: the rows that take a restart differ from it once changed)
}

bool resolution_auto() { return opts.resolution == 0; }
int resolution_scale() {
    const float s = upscaler_rows::percent(opts.resolution) / 100.0f;
    return s > 0 ? (int)(twelfths(s) * 10000.0f / 12.0f + 0.5f) : 0;   // (the twelfth game_size labels)
}

void menu_set_gpu_count(int count) {
    for (Row& r : rows) {
        if (r.value == &opts.gpu_device) {
            r.hi = count;
            if (opts.gpu_device > count) opts.gpu_device = 0;
        }
    }
    restart_init();
    for (int i = 0; i < kRows; i++)   // (the row's value the game started with, the cards counted)
        if (rows[i].value == &opts.gpu_device) restart_state.restart_value(restart_slot[i], opts.gpu_device);
}

bool menu_is_open() { return open_now.load(std::memory_order_relaxed); }
// (the tools off on the Debug tab: flight, the free camera and heart protection put away; -1, heart protection never set, is the
// host's own HOS_OOT_PROTECT_HEARTS)
int menu_heart_protection() {
    const int v = protect_hearts.load(std::memory_order_relaxed);
    return cheats_on() || v < 0 ? v : 0;
}
int menu_player_flight() { return cheats_on() ? player_flight.load(std::memory_order_relaxed) : -1; }
int menu_free_camera() { return cheats_on() ? free_camera.load(std::memory_order_relaxed) : 0; }
int menu_take_cheat_action() { return cheat_action.exchange(0, std::memory_order_relaxed); }

void menu_note_present_sizes(uint32_t picture_w, uint32_t picture_h, uint32_t window_w, uint32_t window_h) {
    const auto c = [](uint32_t v) { return (uint64_t)std::min(v, 0xffffu); };
    present_sizes.store(c(picture_w) << 48 | c(picture_h) << 32 | c(window_w) << 16 | c(window_h), std::memory_order_relaxed);
}
void menu_note_frame(bool game_frame, int shown) {
    if (shown) shown_frames.fetch_add(shown, std::memory_order_relaxed);
    if (game_frame) game_frames.fetch_add(1, std::memory_order_relaxed);
}

// The Fullscreen row, F10 and Alt+Enter are one setting: the row asks the window's thread (host/display.c), Alt+Enter shows in the row
void fullscreen_tick() {
    static int applied = 0, settle = 0;
    const HWND main_window = app_window();
    if (!main_window) return;
    static const bool env = getenv("HOS_FULLSCREEN") != nullptr;   // (a test's own choice: the row is left alone)
    if (env) return;
    if (opts.fullscreen != applied) {
        applied = opts.fullscreen;
        settle = 60;
        PostMessageA(main_window, WM_APP + 7, (WPARAM)applied, 0);
        return;
    }
    if (settle > 0) { settle--; return; }
    const int now = !(GetWindowLongA(main_window, GWL_STYLE) & WS_CAPTION);
    if (now != applied) {
        opts.fullscreen = applied = now;
        menu_refresh = true;
        save();
    }
}

/// A rate over the half-second that just ended. The first call only starts that interval: the frames since launch,
/// divided by one second, were shown as the rate the moment the counter was turned on.
bool fps_take(int64_t now, int64_t shown, int64_t game, int64_t* next, int64_t* last_shown, int64_t* last_game,
              double* shown_rate, double* game_rate) {
    if (now < *next) return false;
    const bool have = *next != 0;
    if (have) {
        const double secs = (now - *next + 500000000) / 1e9;
        *shown_rate = (shown - *last_shown) / secs;
        *game_rate = (game - *last_game) / secs;
    }
    *last_shown = shown;
    *last_game = game;
    *next = now + 500000000;
    return have;
}

void menu_option_set(int id, int value) {
    if (id == HOS_OPT_GAME_OPTIONS) {   // (Triforce: the game's own Options screen runs: each frame it does)
        game_options_ns.store(value ? prof_now() : 0, std::memory_order_relaxed);
        return;
    }
    if (id == OOT_CHEAT_AGE_ID) {   // (Link's age: the Switch age row shows it)
        if (link_age.exchange(value, std::memory_order_relaxed) != value && page == 0 && cur_tab == TAB_DEBUG) menu_refresh = true;
        return;
    }
    if (id >= OOT_CHEAT_ITEM_BASE && id < OOT_CHEAT_ITEM_BASE + kItems) {   // (the game side's inventory counts)
        if (item_counts[id - OOT_CHEAT_ITEM_BASE].exchange(value, std::memory_order_relaxed) != value && page == 4)
            menu_refresh = true;
        return;
    }
    if (id == HOS_OPT_CHEAT_FLIGHT) {
        if (cheats_on()) {
            int expected = value ? 0 : 1;
            player_flight.compare_exchange_strong(expected, value != 0, std::memory_order_relaxed);
        }
        return;
    }
    if (id != 7) return;
    opts.mouse_sens = std::min(std::max(value, 10), 500);   // (the keys work while the menu is closed: it shows it when opened)
    save();
}

/// (hookshot) the mouse on the inventory: a section tab chooses it, a card the item (a click selects it, it does not edit),
/// the page arrows turn the page with the choice, the action button and the amount's halves act; the wheel turns the page
/// shown and leaves the choice where it is; a right click goes back
void inventory_mouse(const MkwVkPointer& ptr, int mx, int my, bool* press, bool& changed) {
    const menu_hit::Hit hit = hits.at((float)mx, (float)my);
    if (ptr.pressed & HOS_MOUSE_LEFT) {
        if (hit.part == menu_hit::kRow && hit.index >= 0 && hit.index <= kItemSections) choose_inventory_filter(hit.index), changed = true;
        else if (hit.part == menu_hit::kListLine && hit.index >= inventory_begin() && hit.index < inventory_end())
            item_choice = hit.index, inventory_amount = false, changed = true;
        else if (hit.part == menu_hit::kButton && hit.index == 0) press[7] = true;
        else if (hit.part == menu_hit::kButton && hit.index == 1 && hit.side) press[hit.side < 0 ? 3 : 4] = true;
        else if (hit.part == menu_hit::kButton && (hit.index == 2 || hit.index == 3))   // (the page arrows)
            press[hit.index == 2 ? 13 : 14] = true, inventory_amount = false;
    }
    if (ptr.pressed & HOS_MOUSE_RIGHT) press[5] = true;
    if (ptr.wheel) {
        const int was = inventory_page;
        inventory_page = std::max(0, inventory_page - ptr.wheel);   // (drawing keeps it to the pages there are)
        changed |= inventory_page != was;
    }
}
/// (hookshot) the mouse on the teleport list: a click on a card selects it (Travel, or confirm, goes), the Travel button
/// goes; the wheel scrolls the cards and leaves the choice where it is; a right click goes back
void teleport_mouse(const MkwVkPointer& ptr, int mx, int my, bool* press, bool& changed) {
    const menu_hit::Hit hit = hits.at((float)mx, (float)my);
    if (ptr.pressed & HOS_MOUSE_LEFT) {
        if (hit.part == menu_hit::kListLine && hit.index >= 0 && hit.index < kTeleports)
            teleport_choice = hit.index, teleport_follow = true, changed = true;
        else if (hit.part == menu_hit::kButton) press[7] = true;
    }
    if (ptr.pressed & HOS_MOUSE_RIGHT) press[5] = true;
    if (ptr.wheel) teleport_wheel += ptr.wheel, teleport_follow = false, changed = true;
}
/// A debug tool's row on the Debug tab (Row::cheat i): Left and Right, and confirm (press[7]: A, Enter, a click). Teleport and
/// Inventory open their pages; back from them comes to the Debug tab, the row still selected
void cheat_press(int i, const bool* press, bool& changed) {
    if (i < 2 && (press[3] || press[4] || press[7])) {   // (Left On, Right Off, as they are drawn; confirm flips)
        const bool was = i == 0 ? protect_hearts.load() == 1 : player_flight.load() >= 0;
        const bool on = press[7] ? !was : press[3];
        if (i == 0) protect_hearts.store(on ? 1 : 0);
        else if (on != was) player_flight.store(on ? 0 : -1);
        changed = true;
    }
    if (i == 4 && (press[3] || press[4])) {
        clock_hour = (clock_hour + (press[4] ? 1 : 23)) % 24;
        changed = true;
    }
    if (i == 5 && (press[3] || press[4])) {
        pouch_item = (pouch_item + kPouchItems + (press[4] ? 1 : -1)) % kPouchItems;
        changed = true;
    }
    if (i == 6 && (press[3] || press[4])) {
        const int step = press[4] ? 1 : -1;
        pouch_delta = std::clamp(pouch_delta + step, -10, 10);
        if (!pouch_delta) pouch_delta = step;
        changed = true;
    }
    if (press[7] && i >= 2 && i <= 4) cheat_action.store(i == 4 ? 100 + clock_hour : i);
    if (press[7] && i == 6) cheat_action.store(200 + pouch_item * 32 + pouch_delta + 10);
    if ((press[3] || press[4] || press[7]) && i == 7) {   // (an On / Off as the first two: Left On, Right Off, confirm flips)
        free_camera.store(press[7] ? !free_camera.load() : press[3] ? 1 : 0);
        changed = true;
    }
    if (press[7] && i == 8) page = 3, teleport_follow = true, changed = true;
    if (press[7] && i == 10) page = 4, choose_inventory_filter(0), changed = true;
    if (i == 9 && (press[3] || press[4])) {   // (Kid | Adult: Left Kid, Right Adult, a switch when Link is the other age)
        const int age = link_age.load(std::memory_order_relaxed);
        if (age >= 0 && age != (press[4] ? 1 : 0)) {
            cheat_action.store(999);
            open_now = false;
            changed = true;
        }
    }
    if (press[7] && i == 9) {
        cheat_action.store(999);   // (host/hos_main.c OOT_AGE_ACTION)
        open_now = false;
        changed = true;
    }
}

void menu_tick() {
    fullscreen_tick();
    enum { K_N = 17 };
    static bool held[K_N];
    static const int keys[K_N] = {VK_INSERT, VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_ESCAPE, VK_F1,   // (F1: for keyboards without Insert)
                                  VK_RETURN, VK_DELETE, VK_BACK,
                                  'Q', 'E', VK_TAB,   // (the tabs: Q and E, Tab and Shift+Tab)
                                  VK_PRIOR, VK_NEXT, VK_HOME, VK_END};   // (the long lists: a page, the ends)
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    const bool mine = pid == GetCurrentProcessId();
    bool press[K_N] = {};
    for (int i = 0; i < K_N; i++) {
        const bool down = mine && (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
        const bool queued = host && host->take_key_press && host->take_key_press(keys[i]);
        // Read both keys even if one is disabled, so old presses cannot toggle the menu after a change.
        // (F1 always opens the menu; Menu shortcut's F1 only leaves Insert to an overlay)
        const bool enabled = keys[i] != VK_INSERT || opts.menu_shortcut == 0;
        press[i] = enabled && (host && host->take_key_press ? mine && queued : down && !held[i]);
        held[i] = down;
        if (press[i]) pad_last = false;   // (the keys: their names in the prompts)
    }
    // (hookshot) the gamepads, by position: Home, or Select and Start together, as Insert; the D-pad as the arrows; the
    // buttons the Switch's A and B are bound to as Enter and Esc, as in the game (vk_menu_keys.h; with the Nintendo layout A
    // is the right face button), not while a binding waits for its button
    static uint32_t pad_held = 0;
    const uint32_t raw = mine && host && host->pad_raw ? host->pad_raw() : 0, pad_press = raw & ~pad_held;
    const uint32_t chord = 1u << HOS_PAD_BACK | 1u << HOS_PAD_START;
    const bool chord_now = (raw & chord) == chord && (pad_held & chord) != chord;
    pad_held = raw;
    if (pad_press) pad_last = auto_pad = true;   // (a controller: its buttons in the prompts, and Menu size Auto's choice)
    {   // (RX_VK_UI_PROMPTS=pad or keys: the prompts for screenshots of test runs, which have no controller and no focus)
        static const char* force = getenv("RX_VK_UI_PROMPTS");
        if (force) pad_last = !strcmp(force, "pad");
    }
    auto pad = [&](int p) { return listening != 1 && (pad_press >> p & 1) != 0; };
    if (host && host->binding)   // (A and B as bound now: the Controls page or the Layout preset can change them)
        menu_keys::confirm_back(host->binding(HOS_BIND_PAD, HOS_SW_A, 0), host->binding(HOS_BIND_PAD, HOS_SW_B, 0), &pad_confirm,
                                &pad_back);
    if (pad(HOS_PAD_GUIDE) || chord_now) press[0] = true;
    {   // holding an arrow or the D-pad repeats it: after 0.35 s, 12 a second, then 30 a second after 1.5 s held
        static int64_t held_since[5], next_repeat[5];
        const int pads[5] = {0, HOS_PAD_UP, HOS_PAD_DOWN, HOS_PAD_LEFT, HOS_PAD_RIGHT};
        const int64_t t = prof_now();
        for (int i = 1; i <= 4; i++) {
            const bool down = held[i] || (open_now.load() && listening != 1 && (raw >> pads[i] & 1));
            if (!down) { held_since[i] = 0; continue; }
            if (!held_since[i]) { held_since[i] = t, next_repeat[i] = t + 350000000LL; continue; }
            if (t >= next_repeat[i] && open_now.load() && listening == 0) {
                press[i] = true;
                next_repeat[i] = t + (t - held_since[i] > 1500000000LL ? 33000000LL : 83000000LL);
            }
        }
    }
    if (open_now.load() && listening != 1) {
        press[1] |= pad(HOS_PAD_UP), press[2] |= pad(HOS_PAD_DOWN), press[3] |= pad(HOS_PAD_LEFT);
        press[4] |= pad(HOS_PAD_RIGHT), press[5] |= pad(pad_back), press[7] |= pad(pad_confirm);
    }
    bool script_altg = false;
    int script_tab = 0;   // (a test's TABL or TABR: on the game's own tab too, where Q and E are not ours)
    {   // RX_VK_MENU_SCRIPT (test runs, which never have the focus): timed menu keys, "<seconds>:<INS|UP|DOWN|LEFT|RIGHT|ESC|ENTER|TABL|TABR>,...";
        // and the gamepad's face buttons by position, PADS PADE PADW PADN (bottom, right, left, top), as a controller's press
        static const char* next = getenv("RX_VK_MENU_SCRIPT");
        static const int64_t start = prof_now();
        double at;
        char key[8];
        int n = 0;
        if (next && sscanf(next, "%lf:%7[A-Z]%n", &at, key, &n) == 2 && (prof_now() - start) / 1e9 >= at) {
            static const char* names[] = {"INS", "UP", "DOWN", "LEFT", "RIGHT", "ESC", "", "ENTER", "", "", "TABL", "TABR"};
            for (int i = 0; i < 12; i++) press[i] |= *names[i] && !strcmp(key, names[i]);
            script_altg = !strcmp(key, "ALTG");
            script_tab = !strcmp(key, "TABL") ? -1 : !strcmp(key, "TABR") ? 1 : 0;
            static const char* pads[] = {"PADS", "PADE", "PADW", "PADN"};
            for (int p = 0; p < 4; p++)
                if (!strcmp(key, pads[p]) && open_now.load() && listening != 1) {
                    press[5] |= p == pad_back, press[7] |= p == pad_confirm;
                    pad_last = auto_pad = true;
                }
            next = next[n] == ',' ? next + n + 1 : nullptr;
        }
    }
    {   // (Triforce) Alt+G: the Debug tab unlocked, or put away again with its tools off; while the game's window has the focus,
        // the options open or not (RX_VK_MENU_SCRIPT's ALTG too)
        static bool g_held = false;
        const bool g_down = mine && (GetAsyncKeyState('G') & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000);
        if ((g_down && !g_held) || script_altg) {
            debug_unlocked = !debug_unlocked;
            if (!debug_unlocked) {
                opts.debug_tools = 0;   // (nothing hidden stays on: flight, the free camera and heart protection go with the tab)
                if (cur_tab == TAB_DEBUG) cur_tab = TAB_DISPLAY;
                if (page == 3 || page == 4) page = 0;
            }
            save();
            logf("menu: the Debug tab %s (Alt+G)", debug_unlocked ? "unlocked" : "locked");
            snprintf(notice_text, sizeof notice_text, "%s", debug_unlocked ? "Debug options unlocked" : "Debug options locked");
            notice_until = prof_now() + 2500000000LL;
            menu_refresh = true;
        }
        g_held = g_down;
    }
    if (cur_tab == TAB_DEBUG && !debug_shown()) cur_tab = TAB_DISPLAY;
    bool tab_left = press[10] || pad(HOS_PAD_LSHOULDER) || pad(HOS_PAD_LTRIGGER);
    bool tab_right = press[11] || press[12] || pad(HOS_PAD_RSHOULDER) || pad(HOS_PAD_RTRIGGER);
    if (press[12] && (GetAsyncKeyState(VK_SHIFT) & 0x8000)) tab_left = true, tab_right = false;
    // (Triforce) inside the game's own Options screen (see game_options_ns)
    const bool in_game_options = game_options_shown();
    if (!in_game_options) {
        embed_since = 0;
        if (embed_open) {   // (the game's screen went: ours with it)
            embed_open = false;
            if (open_now.load() && page != 5) open_now = false, page = 0, menu_refresh = true;
        }
    } else {
        if (!embed_since) embed_since = prof_now();
        if (open_now.load()) embed_open = true;   // (opened over it with F1 as well)
    }
    if (in_game_options && !open_now.load()) {
        // The game's own tab: the game has the input. L and R as it has them go to our tabs: the gamepad inputs the Switch's L
        // and R are bound to, and their keys (Q, E and Tab are the game's own keys here: E is its A by default)
        bool go[2] = {script_tab < 0, script_tab > 0};
        static bool key_held[2][2];
        for (int side = 0; side < 2 && host && host->binding; side++) {
            const int sw = side ? HOS_SW_R : HOS_SW_L, p = host->binding(HOS_BIND_PAD, sw, 0);
            if (p >= 0 && p < HOS_PAD_N && (pad_press >> p & 1)) go[side] = true, pad_last = auto_pad = true;
            for (int slot = 0; slot < 2; slot++) {
                const int vk = host->binding(HOS_BIND_KEY, sw, slot);
                const bool down = mine && vk && (GetAsyncKeyState(vk) & 0x8000) != 0;
                if (down && !key_held[side][slot]) go[side] = true, pad_last = false;
                key_held[side][slot] = down;
            }
        }
        if (go[0] || go[1]) {
            open_now = true, embed_open = true, page = 0, listening = 0;
            cur_tab = go[1] ? TAB_DISPLAY : tabs_shown() - 1;
            auto_pad = pad_last;
            ui::want_glyphs();
            opts.pad_layout = detect_pad_layout();
            dlss_probe(), fsr4_probe();
            refresh_mics();
            menu_refresh = true;
        }
        tab_left = tab_right = false;   // (taken: the first tick of ours does not step on)
    }
    bool changed = menu_refresh;
    menu_refresh = false;
    // (hookshot) the mouse, while the menu is open: the cursor in the window image's pixels (mx, my), tested against the
    // controls as last drawn (hits, vk_menu_hit.h), whether it moved since the last frame, and its clicks and wheel since then
    MkwVkPointer ptr{};
    int mx = -1, my = -1;
    bool mouse = false, moved = false;
    if (open_now.load() && host && host->pointer) {
        mouse = host->pointer(&ptr) != 0;
        int ix = 0, iy = 0;
        menu_hit::to_image(ptr.x, ptr.y, ptr.width, ptr.height, (int)dev.swap_extent.width, (int)dev.swap_extent.height,
                           &ix, &iy);
        if (mouse) mx = ix - (int)menu_ox, my = iy;   // (the menu's column's pixels: draw_menu)
        static int last_x = -1, last_y = -1;
        moved = mouse && mouse_read && (mx != last_x || my != last_y);   // (not the first read: a cursor resting over the
        last_x = mx, last_y = my, mouse_read = true;                     // menu as it opens leaves the selection alone)
        if (moved) pad_last = false;
        if (!mouse) ptr.pressed = 0, ptr.wheel = 0;
    }
    // (closing with a setting that takes a restart changed: the restart question first, Later already chosen on it)
    auto later = [&] {
        int now[mr::kMax];
        restart_values(now);
        restart_state.choose_later(now);
        open_now = false, page = 0, changed = true;
    };
    // (Triforce) Setup chooses the settings before the game starts, and the menu was hidden for that. 11-10: on by default again,
    // for upstream's new rows that Setup has no place for yet. TRIFORCE_INGAME_MENU=0 (triforce_env.txt) hides it: Insert, F1,
    // Home and Select+Start then open nothing
    // (Triforce, 11-13: off by default now, overlay_allowed above. While the options are open, from inside the game's Options,
    // the keys still close them)
    static const bool menu_allowed = overlay_allowed();
    if ((press[0] || press[6]) && open_now.load() && page == 5) {
        later();
    } else if ((press[0] || press[6]) && open_now.load() && restart_ask()) {
        page = 5, ask_sel = 0, listening = 0, changed = true;
    } else if ((press[0] || press[6]) && (menu_allowed || open_now.load())) {
        open_now = !open_now.load(), changed = true;
        page = 0, listening = 0;
        // (the layout row shows the controls as they are: host/nn_hid.c keeps them, the ini's pad_ lines, and a saved
        // layout is not applied over them at start)
        if (open_now.load()) auto_pad = pad_last;   // (Menu size Auto: by what opened the menu)
        if (open_now.load()) ui::want_glyphs();   // (Triforce: the game's button icons, as it was served them this run)
        if (open_now.load()) opts.pad_layout = detect_pad_layout();
        if (open_now.load()) dlss_probe(), fsr4_probe();   // (whether the Upscaler row can offer DLSS and FSR here: asked once)
        if (open_now.load()) refresh_mics();   // (the microphones as they are now)
    }
    if (open_now.load() && page == 5) {   // (hookshot) the restart question: Later (the default) or Restart now
        if (mouse) {   // (a button under the cursor chosen, a click on it pressed, a right click is Later; no list: the wheel does nothing)
            const menu_hit::Hit hit = hits.at((float)mx, (float)my);
            if (hit.part == menu_hit::kButton && (moved || ptr.pressed) && hit.index != ask_sel) ask_sel = hit.index, changed = true;
            if ((ptr.pressed & HOS_MOUSE_LEFT) && hit.part == menu_hit::kButton) press[7] = true;
            if (ptr.pressed & HOS_MOUSE_RIGHT) press[5] = true;
        }
        if (press[1] || press[2] || press[3] || press[4] || tab_left || tab_right) ask_sel ^= 1, changed = true;
        if (press[5] || (press[7] && ask_sel == 0)) {
            later();
        } else if (press[7]) {   // (Restart now: everything saved, then the window closes as the player would close it)
            logf("menu: restart now, to apply %s", restart_list(", ").c_str());
            save();
            pipeline_cache_save(true);
            memory_trial_orderly_end();   // (an exit on purpose: no verdict on the memory trial)
            open_now = false, page = 0, changed = true;
            PostMessageA(app_window(), WM_APP + 9, 0, 0);
        }
    } else if (open_now.load() && page == 1) {   // (hookshot) the Controls page
        if (!can_bind()) page = 0;
        else if (listening) {
            if (press[5] || prof_now() > listen_end) listening = 0, changed = true;   // (Esc, or nothing for a while)
            else if (listening == 1) {
                if (!listen_armed) listen_armed = (raw & ~(1u << HOS_PAD_GUIDE)) == 0;
                else if (const uint32_t p = pad_press & ~(1u << HOS_PAD_GUIDE)) {
                    int bit = 0;
                    while (!(p >> bit & 1)) bit++;
                    host->bind(HOS_BIND_PAD, controls[csel - kTop].button, 0, bit);
                    listening = 0, changed = true;
                    opts.pad_layout = detect_pad_layout();
                    save();
                }
            } else {
                int down = 0;
                for (int k : bindable_keys())
                    if (mine && (GetAsyncKeyState(k) & 0x8000)) {
                        down = k;
                        break;
                    }
                if (!listen_armed) listen_armed = !down;
                else if (down) {
                    host->bind(HOS_BIND_KEY, controls[csel - kTop].button, 0, down);
                    host->bind(HOS_BIND_KEY, controls[csel - kTop].button, 1, 0);
                    listening = 0, changed = true;
                    save();
                }
            }
        } else {
            if (mouse) {   // (the mouse: the row under it selected, a click on a binding listens for its new input, on
                           // Reset resets; the wheel scrolls the page, a right click goes back)
                const menu_hit::Hit hit = hits.at((float)mx, (float)my);
                const bool option = hit.part == menu_hit::kCtlOption;
                const bool on = option || hit.part == menu_hit::kCtlRow || hit.part == menu_hit::kCtlCell;
                const int at = option ? hit.index : kTop + hit.index;   // (the page's row: its own rows first)
                if (on && (moved || ptr.pressed)) {
                    if (at != csel) csel = at, changed = true;
                    if (hit.part == menu_hit::kCtlCell && !keys_only(hit.index) && ccol != hit.side) ccol = hit.side, changed = true;
                }
                if (ptr.pressed & HOS_MOUSE_LEFT) {
                    if (option) press[hit.side < 0 ? 3 : 4] = true;   // (a value's halves: Left and Right; the name: Right)
                    else if (hit.part == menu_hit::kCtlCell || (hit.part == menu_hit::kCtlRow && hit.index == kControls))
                        press[7] = true;   // (as Enter)
                }
                if (ptr.pressed & HOS_MOUSE_RIGHT) press[5] = true;
                if (ptr.wheel) wheel_rows[kControlsSlot] += ptr.wheel, changed = true;   // (scrolls; the selection stays)
            }
            if (press[5]) page = 0, changed = true;
            const int n = kTop + kControls + 1, i = csel - kTop;   // (i: the control's, or reset's)
            if (press[1]) csel = (csel + n - 1) % n, changed = true;
            if (press[2]) csel = (csel + 1) % n, changed = true;
            if (i < 0 && (press[3] || press[4] || press[7])) {   // (the page's own rows: Enter as Right)
                Row& r = rows[tab_row_to_global(TAB_PAGE, csel)];   // (Custom is only what the bindings are: set_row skips it)
                set_row(r, *r.value + (press[3] ? -r.step : r.step), press[3] ? -1 : 1);
                changed = true;
            } else if (i >= 0) {
                if ((press[3] || press[4]) && i < kControls && !keys_only(i)) ccol ^= 1, changed = true;
                if (keys_only(i)) ccol = 1;   // (the stick's and the camera's directions: keys only)
                if (press[7]) {
                    if (i == kControls) {
                        host->bind(HOS_BIND_PAD, -1, 0, 0);
                        opts.pad_layout = 0;
                        save();
                    }
                    else listening = ccol ? 2 : 1, listen_armed = false, listen_end = prof_now() + 6000000000LL;
                    changed = true;
                }
                if ((press[8] || press[9]) && i < kControls) {
                    if (ccol == 0) {
                        host->bind(HOS_BIND_PAD, controls[i].button, 0, -1);
                        opts.pad_layout = detect_pad_layout();
                    }
                    else for (int k = 0; k < 2; k++) host->bind(HOS_BIND_KEY, controls[i].button, k, 0);
                    changed = true;
                    save();
                }
            }
        }
    } else if (open_now.load() && page == 4) {   // the inventory
        const int chosen = item_choice;   // (the page follows the choice when it moves; the wheel's page stays otherwise)
        if (mouse) inventory_mouse(ptr, mx, my, press, changed);
        if (press[5] && inventory_amount) inventory_amount = false, changed = true;   // (back: out of the amount first)
        else if (press[5]) page = 0, changed = true;   // (to the Debug tab, Inventory still selected)
        else if (tab_left || tab_right) {   // (another section, from editing an amount too: that ends)
            const int filters = kItemSections + 1;
            choose_inventory_filter((inventory_filter + filters + (tab_left ? -1 : 1)) % filters);
            changed = true;
        } else if (inventory_amount) {
            if (press[7]) inventory_amount = false, changed = true;
            else if (press[3] || press[4]) cheat_action.store(OOT_CHEAT_ITEM_ACTION + item_choice * 256 + 64 + (press[4] ? 1 : -1));
        } else {
            // (the cards as drawn, inventory_columns to a row: the arrows move, Page Up and Down a page, Home and End the ends)
            const int begin = inventory_begin(), end = inventory_end();
            const int step = (press[4] - press[3]) + (press[2] - press[1]) * inventory_columns + (press[14] - press[13]) * inventory_capacity;
            if (step || press[15] || press[16]) {
                item_choice = press[15] ? begin : press[16] ? end - 1 : std::clamp(item_choice + step, begin, end - 1);
                inventory_reveal();
                changed = true;
            } else if (press[7] && item_counts[item_choice].load(std::memory_order_relaxed) >= 0) {
                if (item_kinds[item_choice]) inventory_amount = true, changed = true;   // (an amount: Left and Right change it)
                else cheat_action.store(OOT_CHEAT_ITEM_ACTION + item_choice * 256 + 200);   // (unlock or relock)
            }
            if (item_choice != chosen) inventory_reveal();   // (a click on a card: its page)
        }
    } else if (open_now.load() && page == 3) {   // the teleport list
        if (mouse) teleport_mouse(ptr, mx, my, press, changed);
        if (press[5]) page = 0, changed = true;   // (to the Debug tab, Teleport still selected)
        else if (press[7]) {
            cheat_action.store(1000 + teleport_choice);
            open_now = false, page = 0;   // (back to the game: the map change starts with an event)
            changed = true;
        } else if (tab_left || tab_right) {   // (the first map of the previous or next section)
            const int section = (teleport_section_of(teleport_choice) + kTeleportSections + (tab_left ? -1 : 1)) % kTeleportSections;
            teleport_choice = teleport_sections[section].first, teleport_follow = true;
            changed = true;
        } else {
            // (the cards as drawn, each section's in rows under its heading: Up and Down a row, the column kept; Left and Right the
            // previous and next card; Page Up and Down the rows that fit; Home and End the ends)
            const int rows = (press[2] - press[1]) + (press[14] - press[13]) * teleport_page_rows;
            const int was = teleport_choice;
            if (press[15]) teleport_choice = 0;
            else if (press[16]) teleport_choice = kTeleports - 1;
            else teleport_choice = std::clamp(teleport_step_rows(teleport_choice, rows) + (press[4] - press[3]), 0, kTeleports - 1);
            if (teleport_choice != was) teleport_follow = true, changed = true;
        }
    } else if (open_now.load()) {
        if (mouse) {   // (the mouse: see the top of this file)
            const menu_hit::Hit hit = hits.at((float)mx, (float)my);
            const int ht = hit.part == menu_hit::kTab ? (hit.index == kGameTab ? -1 : hit.index) : hit.part == menu_hit::kTabPrev ? TAB_COUNT
                         : hit.part == menu_hit::kTabNext ? TAB_COUNT + 1 : -1;
            if (ht != hover_tab) hover_tab = ht, changed = true;
            int& sel = tab_selected[cur_tab];
            const bool on_row = hit.part == menu_hit::kRow || hit.part == menu_hit::kSlider;
            if (drag_row >= 0) {   // (a slider held: it follows the cursor's x until let go)
                Row& d = rows[tab_row_to_global(cur_tab, drag_row)];
                if (!(ptr.held & HOS_MOUSE_LEFT) || !d.value) {
                    drag_row = -1;
                    save();
                } else {
                    const int permille = hits.drag((float)mx, drag_row);
                    const int v = permille < 0 ? *d.value : menu_hit::slider_value(permille, d.lo, d.hi, d.step);
                    if (v != *d.value) set_row(d, v, v > *d.value ? 1 : -1, false), changed = true;
                }
            } else if (on_row && (moved || ptr.pressed) && hit.index != sel) {
                sel = hit.index, changed = true;
            }
            if ((ptr.pressed & HOS_MOUSE_LEFT) && drag_row < 0) {
                if (hit.part == menu_hit::kTabPrev) tab_left = true;
                else if (hit.part == menu_hit::kTabNext) tab_right = true;
                else if (hit.part == menu_hit::kTab && hit.index == kGameTab) open_now = false, changed = true;   // (the game's own tab)
                else if (hit.part == menu_hit::kTab && hit.index != cur_tab && hit.index < tabs_shown()) cur_tab = hit.index, changed = true;
                else if (hit.part == menu_hit::kSlider) {
                    Row& d = rows[tab_row_to_global(cur_tab, hit.index)];
                    drag_row = hit.index;
                    const int v = menu_hit::slider_value(hit.permille, d.lo, d.hi, d.step);
                    if (v != *d.value) set_row(d, v, v > *d.value ? 1 : -1, false);
                    changed = true;
                } else if (hit.part == menu_hit::kRow) {
                    Row& d = rows[tab_row_to_global(cur_tab, hit.index)];
                    if (!d.value && d.cheat >= 0 && (cheat_chooses(d.cheat) || age_shown(d.cheat) || d.cheat <= 1 || d.cheat == 7) &&
                        hit.side != 0)
                        press[hit.side < 0 ? 3 : 4] = true;   // (a tool's choice: its halves as Left and Right)
                    else if (!d.value) press[7] = true;   // (a page, a tool: as Enter)
                    else if (d.hi - d.lo == d.step || hit.side >= 0) press[4] = true;   // (a toggle flips; the right half, or the name, as Right)
                    else press[3] = true;                                               // (the left half as Left)
                }
            }
            if (ptr.pressed & HOS_MOUSE_RIGHT) press[5] = true;   // (as Esc)
            // (the wheel scrolls the rows, wherever the cursor is; it never changes a value, the selection or the tab: the
            // values change by a click on their arrows or halves, a slider's drag, Left and Right, A and Enter)
            if (ptr.wheel && drag_row < 0) wheel_rows[cur_tab] += ptr.wheel, changed = true;
        }
        if (press[5] && cur_tab == TAB_MOTION) cur_tab = TAB_CONTROLS, press[5] = false, changed = true;   // (back)
        if (press[5] && pending) pending = nullptr, changed = true;   // (a Restart or Quit waiting for its second press: cancelled)
        else if (press[5] && cur_tab == TAB_MOTION) cur_tab = TAB_CONTROLS, press[5] = false, changed = true;   // (back)
        else if (press[5] && restart_ask()) page = 5, ask_sel = 0, changed = true;   // (the restart question first)
        else if (press[5]) open_now = false;
        // (the tabs: LB and RB, Q and E, or Tab; from the Motion page, as from Controls)
        if ((tab_left || tab_right) && cur_tab == TAB_MOTION) cur_tab = TAB_CONTROLS;
        if (tab_left || tab_right) {
            const int n = tabs_shown();
            // (inside the game's Options: past our first or last tab is the game's own again)
            if (embed_open && ((tab_left && cur_tab == 0) || (tab_right && cur_tab == n - 1))) open_now = false;
            else cur_tab = (cur_tab + n + (tab_left ? -1 : 1)) % n;
            changed = true;
        }
        const int tab_count = tab_row_count(cur_tab);
        int& selected = tab_selected[cur_tab];
        if (selected >= tab_count) selected = tab_count - 1;   // (a row gone since: the debug tools with them turned off)
        auto heading_at = [&](int k) { return rows[tab_row_to_global(cur_tab, k)].heading; };   // (stepped over)
        if (heading_at(selected)) selected = (selected + 1) % tab_count;
        if (cur_tab == TAB_DEBUG && (press[1] || press[2])) {   // (the Debug tab: in its layout's order, debug_rows)
            selected = debug_step(selected, press[2] ? 1 : -1);
            changed = true;
        } else {
            if (press[1]) {
                do selected = (selected + tab_count - 1) % tab_count;
                while (heading_at(selected));
                changed = true;
            }
            if (press[2]) {
                do selected = (selected + 1) % tab_count;
                while (heading_at(selected));
                changed = true;
            }
        }
        Row& r = rows[tab_row_to_global(cur_tab, selected)];
        const bool confirm_selected = &r == &rows[kQuitRow] || &r == &rows[kRestartRow];
        if (pending && (pending != &r || !mine)) pending = nullptr, changed = true;
        if (press[7] && confirm_selected) {
            if (mine && !press[5]) {
                if (pending != &r) {
                    pending = &r, changed = true;
                } else {   // (as Restart now: saved first, as the window's own close does not force the pipeline cache)
                    save();
                    pipeline_cache_save(true);
                    memory_trial_orderly_end();
                    pending = nullptr;
                    if (&r == &rows[kRestartRow]) {   // (the restart question's Restart now: the window closes, then the game starts again)
                        logf("menu: restart");
                        open_now = false, changed = true;
                        if (const HWND window = app_window()) PostMessageA(window, WM_APP + 9, 0, 0);
                    } else if (const HWND window = app_window()) {
                        PostMessageA(window, WM_CLOSE, 0, 0);
                    }
                }
            }
        } else if (r.cheat >= 0) cheat_press(r.cheat, press, changed);   // (a debug tool: on the Debug tab itself)
        else if (press[7] && is_motion_row(r)) cur_tab = TAB_MOTION, tab_selected[TAB_MOTION] = 0, press[7] = false, changed = true;
        else if (press[7] && !r.value && can_bind()) page = 1, csel = 0, ccol = 0, changed = true;
        if ((press[3] || press[4] || press[7]) && r.value) {   // (Enter, or A, as Right)
            if (press[7]) press[4] = true;
            set_row(r, *r.value + (press[4] ? r.step : -r.step), press[4] ? 1 : -1);
            changed = true;
        }
        const int64_t now = prof_now();
        if (changed) {   // (a new row or value: its name from the start, after a rest)
            marquee_pos = 0, marquee_next = now + kMarqueeRest;
        } else if (marquee_max > 0 && now >= marquee_next) {
            if (marquee_pos >= marquee_max) marquee_pos = 0, marquee_next = now + kMarqueeRest;   // (back to the start)
            else if (++marquee_pos >= marquee_max) marquee_next = now + kMarqueeRest;            // (rest at the end)
            else marquee_next = now + kMarqueeStep;
            changed = true;
        }
    }
    {   // (hookshot) the window lets the mouse go while the menu is open, and takes it back after (host/display.c)
        static bool told = false;
        const bool now_open = open_now.load();
        if (!now_open) {
            if (drag_row >= 0) save();   // (a slider let go off the window: kept now)
            hover_tab = -1, drag_row = -1, mouse_read = false;
            pending = nullptr;
        }
        if (now_open != told && app_window()) {
            told = now_open;
            PostMessageA(app_window(), WM_APP + 8, (WPARAM)now_open, 0);
        }
    }
    if (opts.show_fps) {
        static int64_t next = 0, last_game = 0, last_shown = 0;
        const int64_t now = prof_now();
        double shown = 0, game = 0;
        if (fps_take(now, shown_frames.load(), game_frames.load(), &next, &last_shown, &last_game, &shown, &game)) {
            snprintf(fps_text, sizeof fps_text, "%.0f", shown);
            if (opts.framegen >= 1) snprintf(fps_sub, sizeof fps_sub, "%.0f", game);
            else fps_sub[0] = 0;
        }
    }
}

void menu_record(VkCommandBuffer cmd, uint32_t index, uint32_t w, uint32_t h) {
    const bool start = startup_on.load(std::memory_order_acquire);
    const bool fps = opts.show_fps && !start;   // (no game frames yet: not "0 FPS" over the start-up screen)
    const bool open = open_now.load(std::memory_order_relaxed);
    const bool note = !open && !start && notice_text[0] && prof_now() < notice_until;
    // (Triforce: the game's own Options screen is up and none of our tabs: its header's tabs alone)
    const int64_t game_seen = game_options_ns.load(std::memory_order_relaxed);
    const bool game_tab = !open && !start && embed_allowed() && game_seen && prof_now() - game_seen < 250000000LL && embed_since;
    if (!start && !fps && !open && !note && !game_tab) {   // (nothing shows: no ImGui frame, no render pass)
        // (but a new window size or Menu size since the last bake: one frame with nothing drawn that bakes the menu's glyphs
        // and measures its page for the new size, so the next open has no hitch whatever was resized while nothing showed)
        if (prebake_needed(w, h) && ui::begin(w, h)) {
            ui::Ctx c = menu_ctx(w, h);
            prebake(c);
            ::mkwvk::rec::boundary(::mkwvk::rec::B_DIRECT);   // (Dear ImGui records into cmd directly, its backend through its own pointers: what this thread recorded through vk_work.h reaches cmd first)
            ui::record(cmd, index, w, h);
            return;
        }
        ui::idle();
        return;
    }
    if (!ui::begin(w, h)) return;
    {   // (the easing's step: the time since the last drawing, a quarter of the way in 1/60 s)
        static int64_t last = 0;
        const int64_t now = prof_now();
        ease_step = last ? std::min(1.0f, (float)((now - last) / 1e9) * 15.0f) : 1.0f;
        last = now;
    }
    ui::Ctx c = menu_ctx(w, h);
    prebake(c);   // (the menu's glyphs at this size, before it opens: no hitch then)
    if (start) {   // (under the menu, in the overlay's own fonts)
        ui::Ctx sc = ui_ctx(w, h);
        draw_startup(sc);
    }
    if (open) draw_menu(c);
    else if (game_tab) draw_game_tab(c);
    if (fps) draw_fps(c, open || game_tab);
    if (note) draw_notice(c);
    ::mkwvk::rec::boundary(::mkwvk::rec::B_DIRECT);   // (Dear ImGui records into cmd directly, its backend through its own pointers: what this thread recorded through vk_work.h reaches cmd first)
    ui::record(cmd, index, w, h);
}

void menu_record_image(VkCommandBuffer cmd, VkImage image, VkImageView view, uint32_t w, uint32_t h) {
    const bool open = open_now.load(std::memory_order_relaxed);
    const bool fps = opts.show_fps && !startup_on.load(std::memory_order_acquire);
    const bool note = !open && notice_text[0] && prof_now() < notice_until;
    const bool game_tab = !open && game_options_shown() && embed_since;
    if (!open && !fps && !note && !game_tab) return;
    if (!ui::begin(w, h)) return;
    ui::Ctx c = menu_ctx(w, h);
    if (open) draw_menu(c);
    else if (game_tab) draw_game_tab(c);
    if (fps) draw_fps(c, open || game_tab);
    if (note) draw_notice(c);
    ::mkwvk::rec::boundary(::mkwvk::rec::B_DIRECT);   // (Dear ImGui records into cmd directly, its backend through its own pointers: what this thread recorded through vk_work.h reaches cmd first)
    ui::record_to(cmd, image, view, w, h);
}

} // namespace mkwvk
