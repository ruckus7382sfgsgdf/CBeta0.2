// (hookshot) How the overlays look (vk_menu.cpp draws the menu, the FPS counter and the start-up screen with these).
// (Triforce) The menu is drawn as the game's own Options screen is: with the game's own textures (the System screens'
// header, column and caption plate, the option rows' plates and cursor: vk_game_tex.h, read from the player's game files at
// run time), its own fonts (Cetra Display, its button icons, its HUD digits: vk_imgui.cpp) and its own layout's numbers
// (UI/Layout2Archive's SystemOption_00, PaOptionBtnL_00, PaOptionBtnLevel_00, SystemOverlay_00, PaGuideKey_00,
// PaCommonBtnThin_00, MenuOverlay_00: positions and sizes in the layout's units, 3840 x 2160 with the origin at the screen's
// centre and y up; a texture's texel is 1.5 units). Where the game's files have no such textures (another game, a romfs
// without them) the same shapes are drawn as plain plates in the same colours.
// Sizes: c.s is pixels per point of a 720-line window (3 layout units); every text sits on a dark plate, white on brown.
#pragma once

#define IMGUI_DEFINE_MATH_OPERATORS
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

#include "imgui.h"
#include "vk_imgui.h"

namespace mkwvk::ui {

constexpr ImU32 rgba(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
inline ImU32 alpha(ImU32 c, float k) {
    const int a = (int)((c >> IM_COL32_A_SHIFT & 0xFF) * k);
    return (c & ~IM_COL32_A_MASK) | ((ImU32)std::clamp(a, 0, 255) << IM_COL32_A_SHIFT);
}

// ---- the palette ----
// (Triforce: the game's menus, measured on its own Options screen: kInk its texts' white (255,253,227), kValue the near-black
// panel a row's value sits on, kValueOn the orange plate of the value chosen (211,144,37) with its text in the same white,
// kDim the other half of a toggle (95,93,81). kType: the game's text sizes as Dear ImGui's: its 54 units draw 65 high.
// The old overlay's golds stay for the start-up screen's bar)
constexpr float kType = 1.2f;
constexpr ImU32 kInk = rgba(255, 253, 227), kInkBright = rgba(255, 253, 227), kSoft = rgba(226, 218, 190), kDim = rgba(104, 100, 86),
                kGold = rgba(222, 152, 40), kGoldHi = rgba(238, 168, 52), kGoldInk = rgba(36, 24, 8), kAccent = rgba(214, 170, 96),
                kEdge = rgba(150, 122, 74, 200), kCellDark = rgba(44, 32, 17, 240), kCellName = rgba(107, 80, 42, 255),
                kCellNameSel = rgba(124, 93, 49, 255), kBronze = rgba(84, 64, 38), kColumn = rgba(44, 36, 26, 244),
                kBannerTop = rgba(150, 122, 78, 255), kBanner = rgba(118, 94, 58, 255), kBannerInk = rgba(255, 251, 238),
                kBannerEdge = rgba(40, 28, 14), kTabInset = rgba(60, 42, 22, 235), kBox = rgba(52, 38, 22, 250),
                kBoxEdge = rgba(122, 98, 64, 220), kBar = rgba(10, 8, 5, 200), kListen = rgba(150, 120, 40),
                kValue = rgba(24, 21, 14, 255), kValueOn = rgba(211, 144, 37, 255), kValueOnInk = rgba(255, 253, 227),
                kCursor = rgba(255, 255, 255), kCursorGlow = rgba(150, 185, 255);
constexpr int kDimAlpha = 178;   // (the game behind the menu: the System screens darken it so)

/// The unit: pixels per point (a 720th of the window's height; 3 of the game's layout units). The game's menus have one
/// size, so the menu has too; *margin: kept for the callers that lay out from an edge (the game's guides sit 20 points in)
inline float unit(float h, int /*size*/, bool /*fullscreen*/, bool /*pad*/, float* margin) {
    *margin = h * (20.0f / 720.0f);
    return h / 720.0f;
}

struct Ctx {
    ImDrawList* d;
    Fonts f;
    float s;      // (pixels per point)
    float w, h;   // (the window, or the menu's 16:9 column of it)
    float safe;   // (the margin kept clear at each edge, pixels)
    bool pad;     // (a controller was the last input: its buttons in the prompts)
    int confirm_dot = 3, back_dot = 1;   // (the face buttons that confirm and go back: vk_menu_keys.h dot, 3 the bottom, 1 the right)
    float pulse = 1;   // (the cursor's glow now, 0 to 1: the game's breathes)
    bool over_game = false;   // (Triforce: drawn over the game's own Options screen: the guides' band hides the game's guides)
};

/// A place of the game's layout (units from the screen's centre, y up) in pixels
inline ImVec2 at(const Ctx& c, float x, float y) { return ImVec2(c.w * 0.5f + x * c.s / 3, c.h * 0.5f - y * c.s / 3); }
inline float lu(const Ctx& c, float units) { return units * c.s / 3; }

inline ImVec2 size_of(ImFont* font, float px, const char* t, float wrap = 0) { return font->CalcTextSizeA(px, FLT_MAX, wrap, t); }
inline float text(Ctx& c, ImFont* font, float px, ImVec2 p, ImU32 col, const char* t, float wrap = 0) {
    c.d->AddText(font, px, p, col, t, nullptr, wrap);
    return size_of(font, px, t, wrap).x;
}
/// Text wrapped at wrap pixels, its lines 1.3 em apart (ImGui's own wrap sets them 1 em apart: too tight for reading); its
/// height. draw false: measured only
inline float para(Ctx& c, ImFont* font, float px, ImVec2 p, ImU32 col, const char* t, float wrap, bool draw = true) {
    const char* end = t + strlen(t);
    float y = 0;
    while (t < end) {
        const char* cut = font->CalcWordWrapPosition(px, t, end, wrap);
        if (cut == t) cut = t + 1;   // (a word wider than the line: a letter at least)
        if (draw) c.d->AddText(font, px, ImVec2(p.x, p.y + y), col, t, cut);
        y += px * 1.3f;
        t = cut;
        while (t < end && (*t == ' ' || *t == '\n')) t++;
    }
    return y > 0 ? y - px * 0.3f : 0;
}
/// The top to draw at for capitals centred on y: from the font's own ascent (fonts differ), a cap height of 0.7 em
inline float mid_top(ImFont* font, float px, float y) { return y - font->GetFontBaked(px)->Ascent + 0.35f * px; }
inline float text_mid(Ctx& c, ImFont* font, float px, float x, float y, ImU32 col, const char* t) {
    return text(c, font, px, ImVec2(x, mid_top(font, px, y)), col, t);
}
inline void text_mid_centre(Ctx& c, ImFont* font, float px, float cx, float y, ImU32 col, const char* t) {
    text_mid(c, font, px, cx - size_of(font, px, t).x * 0.5f, y, col, t);
}
inline void text_mid_right(Ctx& c, ImFont* font, float px, float x, float y, ImU32 col, const char* t) {
    text_mid(c, font, px, x - size_of(font, px, t).x, y, col, t);
}
inline void text_centre(Ctx& c, ImFont* font, float px, ImVec2 p, ImU32 col, const char* t) {
    text(c, font, px, ImVec2(p.x - size_of(font, px, t).x * 0.5f, p.y), col, t);
}
/// Capitals with extra space between the letters (gap in pixels), centred on x
inline void spaced(Ctx& c, ImFont* font, float px, ImVec2 p, ImU32 col, const char* t, float gap) {
    char one[2] = {0, 0};
    float total = 0;
    for (const char* q = t; *q; q++) one[0] = *q, total += size_of(font, px, one).x + gap;
    float x = p.x - (total - gap) * 0.5f;
    for (const char* q = t; *q; q++) {
        one[0] = *q;
        c.d->AddText(font, px, ImVec2(x, p.y), col, one);
        x += size_of(font, px, one).x + gap;
    }
}
/// A soft shadow under a rectangle: layers growing outwards, fading
inline void shadow(Ctx& c, ImVec2 a, ImVec2 b, float spread, float strength, float dy) {
    const int n = 16;
    for (int i = n; i >= 1; i--) {
        const float k = (float)i / n, e = spread * k;
        c.d->AddRectFilled(a - ImVec2(e, e) + ImVec2(0, dy), b + ImVec2(e, e) + ImVec2(0, dy),
                           rgba(0, 0, 0, (int)(255 * strength * (1 - k) * (1 - k) / 3.5f)), e);
    }
}
inline void triangle(Ctx& c, ImVec2 m, float size, int dir, ImU32 col) {   // (dir: 0 left, 1 right, 2 up, 3 down)
    const float a = size * 0.5f, b = size * 0.42f;
    switch (dir) {
    case 0: c.d->AddTriangleFilled(m + ImVec2(b, -a), m + ImVec2(-b, 0), m + ImVec2(b, a), col); break;
    case 1: c.d->AddTriangleFilled(m + ImVec2(-b, -a), m + ImVec2(b, 0), m + ImVec2(-b, a), col); break;
    case 2: c.d->AddTriangleFilled(m + ImVec2(-a, b), m + ImVec2(0, -b), m + ImVec2(a, b), col); break;
    default: c.d->AddTriangleFilled(m + ImVec2(-a, -b), m + ImVec2(a, -b), m + ImVec2(0, b), col); break;
    }
}
inline void diamond(Ctx& c, ImVec2 m, float r, ImU32 col) {
    c.d->AddQuadFilled(m + ImVec2(0, -r), m + ImVec2(r, 0), m + ImVec2(0, r), m + ImVec2(-r, 0), col);
}
/// A divider: a fine faint line with a small flourish of two mirrored curls at its middle (the start-up screen's)
inline void flourish(Ctx& c, float x0, float x1, float y, ImU32 col) {
    const float s = c.s, m = (x0 + x1) * 0.5f, t = std::max(1.0f, 0.8f * s);
    c.d->AddLine(ImVec2(x0, y), ImVec2(m - 30 * s, y), alpha(col, 0.35f), t);
    c.d->AddLine(ImVec2(m + 30 * s, y), ImVec2(x1, y), alpha(col, 0.35f), t);
    for (int side = -1; side <= 1; side += 2) {
        const float k = (float)side;
        c.d->AddBezierCubic(ImVec2(m + k * 4 * s, y), ImVec2(m + k * 11 * s, y - 7 * s), ImVec2(m + k * 21 * s, y - 6 * s),
                            ImVec2(m + k * 19 * s, y), alpha(col, 0.8f), 1.3f * s);
        c.d->AddBezierCubic(ImVec2(m + k * 19 * s, y), ImVec2(m + k * 17 * s, y + 5 * s), ImVec2(m + k * 11 * s, y + 4 * s),
                            ImVec2(m + k * 12 * s, y + 1 * s), alpha(col, 0.8f), 1.3f * s);
    }
    diamond(c, ImVec2(m, y), 2.2f * s, alpha(col, 0.8f));
}
/// A gold fill, lighter at its top (the start-up screen's bar)
inline void gold(Ctx& c, ImVec2 a0, ImVec2 b0) { c.d->AddRectFilledMultiColor(a0, b0, kGoldHi, kGoldHi, kGold, kGold); }

// ---- (Triforce) the game's textures ----
/// A texture over a..b, whole, tinted (flip: mirrored left to right)
inline void image(Ctx& c, const Art* t, ImVec2 a, ImVec2 b, ImU32 col = IM_COL32_WHITE, bool flip = false) {
    if (t) c.d->AddImage(t->tex->GetTexRef(), a, b, ImVec2(flip ? 1.0f : 0.0f, 0), ImVec2(flip ? 0.0f : 1.0f, 1), col);
}
/// A texture over a..b in nine parts, as the game's window panes: its borders (texels: left and right, top and bottom) keep
/// their size, a texel 1.5 layout units; the middle stretches. A rectangle smaller than its borders shrinks them
inline void nine(Ctx& c, const Art* t, ImVec2 a, ImVec2 b, float bx, float by, ImU32 col = IM_COL32_WHITE) {
    if (!t) return;
    const float k = c.s * 0.5f;   // (pixels a texel: 1.5 units)
    float px = bx * k, py = by * k;
    px = std::min(px, (b.x - a.x) * 0.5f), py = std::min(py, (b.y - a.y) * 0.5f);
    const float xs[4] = {a.x, a.x + px, b.x - px, b.x}, ys[4] = {a.y, a.y + py, b.y - py, b.y};
    const float us[4] = {0, bx / t->w, 1 - bx / t->w, 1}, vs[4] = {0, by / t->h, 1 - by / t->h, 1};
    for (int j = 0; j < 3; j++)
        for (int i = 0; i < 3; i++) {
            if (xs[i + 1] <= xs[i] || ys[j + 1] <= ys[j]) continue;
            c.d->AddImage(t->tex->GetTexRef(), ImVec2(xs[i], ys[j]), ImVec2(xs[i + 1], ys[j + 1]), ImVec2(us[i], vs[j]), ImVec2(us[i + 1], vs[j + 1]), col);
        }
}

/// A row's plate over a..b (what shows of it: the texture's soft rim goes 28 units further out, as the layout's 1880 x 180
/// pane shows 1824 x 124): the game's BtnThinBase plate with its fine edge line; btn: the buttons' frame, with the curls
inline void plate(Ctx& c, ImVec2 a, ImVec2 b, bool sel = false, bool btn = false) {
    const Art* t = art(btn ? "plate_btn" : sel ? "plate_row_sel" : "plate_row");
    const float m = lu(c, 28);
    if (t) {
        nine(c, t, a - ImVec2(m, m), b + ImVec2(m, m), 180, 56);
        return;
    }
    c.d->AddRectFilled(a, b, kCellName);
    c.d->AddRect(a, b, kEdge, 0, std::max(1.0f, lu(c, 3)));
}
/// A value's plate over a..b (what shows of it): the game's OptionBtnThinElement, near black under a row's value; on: the
/// orange plate of the value chosen. Its texture's rim is 16 x 14 units outside what shows
inline void value_plate(Ctx& c, ImVec2 a, ImVec2 b, bool on = false, float k = 1) {
    const ImU32 col = alpha(on ? kValueOn : kValue, k);
    const ImVec2 m(lu(c, 16.5f), lu(c, 14));
    if (const Art* t = art("OptionBtnThinElement_00")) nine(c, t, a - m, b + m, 200, 28, col);
    else c.d->AddRectFilled(a, b, col, lu(c, 4));
}
/// The cursor: the game's white hand-drawn frame, its inner edge on a..b, and its blue-white glow outside, which breathes
/// (vk_game_tex.h make_cursor, from OptionBtnThinCursorLine and OptionBtnThinCursorGlow: the layout draws the 640 x 173
/// texture 960 x 260 units, the line's inner edge then a box of 809 x 114)
inline void cursor(Ctx& c, ImVec2 a, ImVec2 b) {
    const Art *line = art("cursor_line"), *glow = art("cursor_glow");
    const ImVec2 m(lu(c, 75.5f), lu(c, 73));
    if (line && glow) {
        nine(c, glow, a - m, b + m, 150, 80, alpha(kCursorGlow, 0.50f + 0.35f * c.pulse));
        nine(c, line, a - m, b + m, 150, 80, kCursor);
        return;
    }
    const float t = lu(c, 13);
    for (int i = 4; i >= 1; i--)
        c.d->AddRect(a - ImVec2(t + i * 1.2f * c.s, t + i * 1.2f * c.s), b + ImVec2(t + i * 1.2f * c.s, t + i * 1.2f * c.s), alpha(kCursorGlow, 0.08f + 0.05f * c.pulse),
                     2 * c.s, 2.2f * c.s);
    c.d->AddRect(a - ImVec2(t * 0.5f, t * 0.5f), b + ImVec2(t * 0.5f, t * 0.5f), kCursor, c.s, t);
}
/// (the old name: the selection's outline)
inline void glow(Ctx& c, ImVec2 a0, ImVec2 b0) { cursor(c, a0, b0); }
/// The game's arrow at m, pointing left or right: on its Options screen a plain white triangle, 18 x 22 units
inline void arrow(Ctx& c, ImVec2 m, bool right, ImU32 col = kInk) { triangle(c, m, lu(c, 28), right ? 1 : 0, col); }   // (28: its anti-aliased rim counted)
/// The game's separator across x0..x1 at y: its mark in the middle, a fine line to either side (PaSlideListSeparator)
inline void separator(Ctx& c, float x0, float x1, float y) {
    const float m = (x0 + x1) * 0.5f, mw = lu(c, 171), mh = lu(c, 60);
    const ImU32 col = rgba(104, 80, 46);   // (the game's: a dim tan on the column, 71,53,32 over 50,36,23)
    if (const Art* t = art("OptionSeparateMark_00")) image(c, t, ImVec2(m - mw, y - mh), ImVec2(m + mw, y + mh), col);
    else diamond(c, ImVec2(m, y), lu(c, 14), col);
    const Art* line = art("SystemLine_03");
    const float gap = lu(c, 150), lh = lu(c, 32);
    if (line) {
        image(c, line, ImVec2(x0, y - lh), ImVec2(m - gap, y + lh), col, false);
        image(c, line, ImVec2(m + gap, y - lh), ImVec2(x1, y + lh), col, true);
    } else {
        c.d->AddLine(ImVec2(x0, y), ImVec2(m - gap, y), col, std::max(1.0f, lu(c, 3)));
        c.d->AddLine(ImVec2(m + gap, y), ImVec2(x1, y), col, std::max(1.0f, lu(c, 3)));
    }
}

// ---- the button prompts: the game's own icons (its External font), or the keys' caps ----
enum Glyph { kUpDown, kLeftRight, kTabs, kConfirm, kBack, kKey };
struct Prompt {
    Glyph g;
    const char* what;
    const char* key = nullptr;   // (kKey: a key's name, shown with the keys and with a controller alike)
};
// (the game's icon font: the Switch's buttons' code points, StaticMsg/GuideKeyIcon.msbt; the host's font points each at the
// icon of the pad input bound to it, host/prompts.c)
constexpr const char *kIconA = "\xEE\x81\x80", *kIconB = "\xEE\x81\x81", *kIconL = "\xEE\x81\x84", *kIconR = "\xEE\x81\x85",
                     *kIconUpDown = "\xEE\x81\xA4", *kIconLeftRight = "\xEE\x81\xA5";
/// One prompt's icon at p (its left; the line's middle my); its width. draw false: measured only
inline float glyph(Ctx& c, float my, float x, const Prompt& pr, bool draw) {
    const float hh = lu(c, 62), kpx = lu(c, 42), gap = lu(c, 8);
    auto key = [&](float kx, const char* label) {   // (a key's cap: white, its name dark, as the icons are)
        const float w = std::max(hh, size_of(c.f.strong, kpx, label).x + lu(c, 30));
        if (draw) {
            c.d->AddRectFilled(ImVec2(kx, my - hh * 0.5f), ImVec2(kx + w, my + hh * 0.5f), kInk, lu(c, 14));
            text_mid_centre(c, c.f.strong, kpx, kx + w * 0.5f, my, rgba(40, 30, 20), label);
        }
        return w;
    };
    if (pr.g == kKey) return key(x, pr.key);
    if (!c.pad || !c.f.glyph) {
        if (c.pad) {   // (a controller, and no icon font: the buttons by name)
            switch (pr.g) {
            case kUpDown: return key(x, "D-pad");
            case kLeftRight: return key(x, "D-pad");
            case kTabs: return key(x, "L") + gap + key(x + key(x, "L") + gap, "R");
            case kConfirm: return key(x, "A");
            default: return key(x, "B");
            }
        }
        switch (pr.g) {
        case kUpDown: {
            const float w = key(x, "\xE2\x86\x91");   // (the arrows: drawn below, the fonts have none)
            (void)w;
            if (draw) {
                c.d->AddRectFilled(ImVec2(x, my - hh * 0.5f), ImVec2(x + hh, my + hh * 0.5f), kInk, lu(c, 14));
                c.d->AddRectFilled(ImVec2(x + hh + gap, my - hh * 0.5f), ImVec2(x + 2 * hh + gap, my + hh * 0.5f), kInk, lu(c, 14));
                triangle(c, ImVec2(x + hh * 0.5f, my), lu(c, 26), 2, rgba(40, 30, 20));
                triangle(c, ImVec2(x + hh * 1.5f + gap, my), lu(c, 26), 3, rgba(40, 30, 20));
            }
            return 2 * hh + gap;
        }
        case kLeftRight: {
            if (draw) {
                c.d->AddRectFilled(ImVec2(x, my - hh * 0.5f), ImVec2(x + hh, my + hh * 0.5f), kInk, lu(c, 14));
                c.d->AddRectFilled(ImVec2(x + hh + gap, my - hh * 0.5f), ImVec2(x + 2 * hh + gap, my + hh * 0.5f), kInk, lu(c, 14));
                triangle(c, ImVec2(x + hh * 0.5f, my), lu(c, 26), 0, rgba(40, 30, 20));
                triangle(c, ImVec2(x + hh * 1.5f + gap, my), lu(c, 26), 1, rgba(40, 30, 20));
            }
            return 2 * hh + gap;
        }
        case kTabs: {
            const float w = key(x, "Q");
            return w + gap + key(x + w + gap, "E");
        }
        case kConfirm: return key(x, "Enter");
        default: return key(x, "Esc");
        }
    }
    // (the game's icons: PaGuideKey's T_Icon, the External font at 102 units)
    const float px = lu(c, 102);
    auto icon = [&](float ix, const char* code) {
        const ImVec2 sz = size_of(c.f.glyph, px, code);
        if (draw) c.d->AddText(c.f.glyph, px, ImVec2(ix, my - sz.y * 0.5f + lu(c, 2)), kInk, code);
        return sz.x;
    };
    switch (pr.g) {
    case kUpDown: return icon(x, kIconUpDown);
    case kLeftRight: return icon(x, kIconLeftRight);
    case kTabs: {
        const float w = icon(x, kIconL);
        return w + lu(c, 4) + icon(x + w + lu(c, 4), kIconR);
    }
    case kConfirm: return icon(x, kIconA);
    default: return icon(x, kIconB);
    }
}
/// The prompts from x on, the label before its icon as the game's own (PaGuideKey: the label 48 units): their width (draw
/// false: measured only)
inline float prompts(Ctx& c, float x, float my, const Prompt* list, int n, bool draw) {
    const float px = lu(c, 48 * kType), x0 = x, gap = lu(c, 20), between = lu(c, 76);
    for (int i = 0; i < n; i++) {
        if (draw) text_mid(c, c.f.body, px, x, my, kInk, list[i].what);
        x += size_of(c.f.body, px, list[i].what).x + gap;
        x += glyph(c, my, x, list[i], draw) + between;
    }
    return n ? x - x0 - between : 0;
}
/// The prompt strip's top: the same at any prompts (the menu lays out above it before it draws the strip, last, on top)
inline float prompt_bar_top(const Ctx& c) { return at(c, 0, -1080 + 160).y; }
/// The prompts at the bottom right, as the System screens' guides: the dark band fading to the left (SystemGuideBase), the
/// labels and icons ending 80 units from the right edge, their middle 62 units up; its top (prompt_bar_top). Prompts wider
/// than the window take smaller ones
inline float prompt_bar(Ctx& c, const Prompt* list, int n) {
    const float my = at(c, 0, -1018).y, right = at(c, 1840, 0).x;
    Ctx k = c;
    const float room = c.w - lu(c, 160), full = prompts(c, 0, my, list, n, false);
    if (full > room) k.s = c.s * room / full;
    const float pw = prompts(k, 0, my, list, n, false);
    const float band0 = std::max(0.0f, right - pw - lu(c, 300)), top = prompt_bar_top(c);
    if (c.over_game)   // (the game's own guides are under it: its band's dark, whole, from where they begin)
        c.d->AddRectFilled(ImVec2(std::min(band0 + lu(c, 260), at(c, 1000, 0).x), top + lu(c, 26)), ImVec2(c.w, c.h), rgba(24, 22, 12));
    if (const Art* t = art("SystemGuideBase_00")) image(c, t, ImVec2(band0, top), ImVec2(c.w + lu(c, 4), c.h), rgba(0, 0, 0, 215), true);
    else c.d->AddRectFilledMultiColor(ImVec2(band0, top), ImVec2(c.w, c.h), alpha(kBar, 0), kBar, kBar, alpha(kBar, 0));
    prompts(k, right - pw, my, list, n, true);
    return top;
}
/// The System screens' header across the top (SystemHeader_00: 200 units high, the picture's ragged lower edge and the
/// shadow under it), its title in the middle when there is one (the "Special" font, 54 units, 90 units down)
inline void banner(Ctx& c, float /*hh*/, const char* title = nullptr) {
    const ImVec2 a = at(c, -1920, 1080), b = at(c, 1920, 880);
    c.d->AddRectFilledMultiColor(ImVec2(a.x, b.y - lu(c, 10)), ImVec2(b.x, b.y + lu(c, 46)), rgba(0, 0, 0, 150), rgba(0, 0, 0, 150), rgba(0, 0, 0, 0),
                                 rgba(0, 0, 0, 0));   // (P_UpSh: the shadow it casts)
    if (const Art* t = art("SystemHeader_00")) image(c, t, a, b);
    else {
        c.d->AddRectFilledMultiColor(a, ImVec2(b.x, b.y - lu(c, 14)), kBannerTop, kBannerTop, kBanner, kBanner);
        c.d->AddRectFilled(ImVec2(a.x, b.y - lu(c, 14)), ImVec2(b.x, b.y - lu(c, 8)), kBannerEdge);
    }
    if (title) text_mid_centre(c, c.f.strong, lu(c, 54 * kType), c.w * 0.5f, at(c, 0, 990).y, kBannerInk, title);
}
/// The header's height as the pages lay out under it
inline float banner_height(const Ctx& c) { return lu(c, 200); }
/// The System screens' dark column from x0 to x1 (pixels), the whole height: SystemBg_05, its soft edges
inline void column_bg(Ctx& c, float x0, float x1) {
    if (const Art* t = art("SystemBg_05")) nine(c, t, ImVec2(x0, -lu(c, 160)), ImVec2(x1, c.h + lu(c, 160)), 80, 242);
    else c.d->AddRectFilled(ImVec2(x0 + lu(c, 30), 0), ImVec2(x1 - lu(c, 30), c.h), kColumn);
}
/// A box (the help, a question): the game's caption plate (SystemCaptionBg_00)
inline void box(Ctx& c, ImVec2 a, ImVec2 b) {
    const float m = lu(c, 82);   // (the plate's own soft rim: the layout's 1800 x 640 pane shows 476 units of height)
    if (const Art* t = art("SystemCaptionBg_00")) nine(c, t, a - ImVec2(m, m), b + ImVec2(m, m), 133, 133);
    else {
        shadow(c, a, b, 18 * c.s, 0.8f, 4 * c.s);
        c.d->AddRectFilled(a, b, kBox);
        c.d->AddRect(a, b, kBoxEdge, 0, 1.5f * c.s);
    }
}
/// A button as the game's own (PaCommonBtnThin: the plate with the curls at its corners, 672 x 144 units of it showing, its
/// text 54 units, the cursor round it when chosen)
inline void button(Ctx& c, ImVec2 a0, ImVec2 b0, const char* label, bool sel) {
    plate(c, a0, b0, false, true);
    text_mid_centre(c, c.f.body, lu(c, 54 * kType), (a0.x + b0.x) * 0.5f, (a0.y + b0.y) * 0.5f - lu(c, 2), kInk, label);
    if (sel) cursor(c, a0 + ImVec2(lu(c, 8), lu(c, 8)), b0 - ImVec2(lu(c, 8), lu(c, 8)));   // (the line on the plate's edge, as the game's)
}

} // namespace mkwvk::ui
