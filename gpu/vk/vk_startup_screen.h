// (triforce) The start-up screen while the shaders are prepared (vk_main.cpp prepare_with_screen; vk_menu.cpp draw_startup
// calls draw): "the relic in a dark hall", the look the start client has (setup/, the tokens in the lab's ui/style_tokens.md).
// The crest (assets/triforce.png, vk_imgui.h logo) is the only picture: a warm near-black hall, one soft light, gold hairlines,
// quiet type. Its three stones are the only saturated things and carry the state: a stone a part of the preparation, unlit
// until the part is done (vk_progress.h stones_lit); the unlit crest and the stones' own pixels are made here from the crest,
// once, with the steps the start client uses, so both show the same thing. Under it one line of progress with what is being
// done, how far, and how long it has left as far as the measured rate says.
// Drawn with ImGui's draw lists alone (no shader of ours: the glows are meshes with a colour a vertex); sizes are the tokens'
// pixels of a 640-line window. Everything eases on the caller's clock, so a test can draw any moment of it.
#pragma once

#define IMGUI_DEFINE_MATH_OPERATORS
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"   // (RegisterUserTexture; the baked glyphs)
#include "vk_progress.h"
#include "vk_ui.h"

namespace mkwvk::start_screen {

// ---- the tokens: colour ----
constexpr ImU32 hex(unsigned rgb, int a = 255) { return IM_COL32(rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255, a); }
constexpr unsigned kInk900 = 0x0A0908, kInk600 = 0x262017, kLine = 0x2C2518, kTextHi = 0xF3EAD5, kText = 0xCDC2A8, kTextDim = 0x948A70,
                   kTextFaint = 0x5F5744, kGold100 = 0xFFF4CC, kGold300 = 0xF7DC8C, kGold500 = 0xE0B23E, kTopLight = 0xFFECAA;
// (the stones in the order they count: emerald, sapphire, ruby)
constexpr unsigned kStoneGlow[3] = {0x7CFF9A, 0x8FC0FF, 0xFF7A84};
constexpr float kStoneAt[3][2] = {{0.53f, 0.32f}, {0.71f, 0.68f}, {0.30f, 0.64f}};   // (their centres in the crest's box)

inline float clamp01(float t) { return t < 0 ? 0 : t > 1 ? 1 : t; }
inline float ease_out(float t) { t = 1 - clamp01(t); return 1 - t * t * t * t * t; }   // (cubic-bezier(0.22, 1, 0.36, 1))
inline float ease_io(float t) { return 0.5f - 0.5f * std::cos(3.14159265f * clamp01(t)); }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
/// A loop between lo and 1, period seconds a cycle, phase in cycles
inline float breath(double now, float period, float lo, float phase = 0) {
    return lerp(lo, 1.0f, 0.5f - 0.5f * (float)std::cos((now / period + phase) * 6.283185307));
}

/// (the window's images blend in linear light: an sRGB surface with no UNORM view, vk_imgui.cpp. A light of alpha a over
/// the dark hall then comes out far brighter than the same alpha blended on the coded values, as everywhere else: its alpha is
/// taken to the power that gives the same light)
inline float blend_power = 1;
inline ImU32 light(unsigned rgb, float a) {
    a = clamp01(a);
    if (blend_power != 1) a = std::pow(a, blend_power);
    return hex(rgb, (int)(a * 255 + 0.5f));
}
inline ImU32 ink(unsigned rgb, float a) { return hex(rgb, (int)(clamp01(a) * 255 + 0.5f)); }

// ---- the crest's layers, made once from its pixels (the start client's steps: setup/Art.cs) ----
struct Art {
    bool tried = false, released = false;
    int w = 0, h = 0;
    ImTextureData* dim = nullptr;       // (the crest, its stones unlit: dark glass)
    ImTextureData* stone[3] = {};       // (each stone's own pixels: over dim at alpha 1 it is lit)
    ImTextureData* shape = nullptr;     // (white in the crest's alpha: the gleam)
    ImTextureData* grain = nullptr;     // (noise: the hall's grain, which also hides the gradients' steps)
};
inline Art art;

inline ImTextureData* texture_of(int w, int h, const unsigned char* rgba) {
    ImTextureData* t = IM_NEW(ImTextureData)();
    t->Create(ImTextureFormat_RGBA32, w, h);
    memcpy(t->GetPixels(), rgba, (size_t)w * h * 4);
    t->UseColors = true;
    ImGui::RegisterUserTexture(t);
    return t;
}
/// A square maximum or minimum over (2r+1)^2, in place
inline void spread(std::vector<unsigned char>& m, int w, int h, int r, bool grow) {
    std::vector<unsigned char> t(m.size());
    for (int pass = 0; pass < 2; pass++) {   // (rows, then columns)
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                unsigned char v = grow ? 0 : 255;
                for (int k = -r; k <= r; k++) {
                    const int xx = pass ? x : x + k, yy = pass ? y + k : y;
                    if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                    const unsigned char s = m[(size_t)yy * w + xx];
                    v = grow ? std::max(v, s) : std::min(v, s);
                }
                t[(size_t)y * w + x] = v;
            }
        m.swap(t);
    }
}
/// The three stones' masks (0 to 1) in a crest of w x h straight RGBA: the stone's hue inside its box, small gaps closed, its
/// holes filled (whatever the outside cannot reach), grown a pixel, feathered. The numbers are for the 512 x 468 crest
inline void stone_masks(const unsigned char* px, int w, int h, std::vector<float> mask[3]) {
    struct Spec { float h0, h1, sat, val; int box[4]; };
    static const Spec specs[3] = {{75, 175, 0.30f, 0.10f, {190, 80, 340, 225}},      // emerald
                                  {190, 265, 0.30f, 0.10f, {265, 225, 460, 410}},    // sapphire
                                  {332, 378, 0.40f, 0.12f, {80, 225, 225, 370}}};    // ruby (hue above 332 or under 18)
    const float sx = w / 512.0f, sy = h / 468.0f;
    for (int k = 0; k < 3; k++) {
        const Spec& sp = specs[k];
        std::vector<unsigned char> m((size_t)w * h, 0);
        const int x0 = (int)(sp.box[0] * sx), y0 = (int)(sp.box[1] * sy), x1 = std::min(w, (int)(sp.box[2] * sx)), y1 = std::min(h, (int)(sp.box[3] * sy));
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++) {
                const unsigned char* p = px + ((size_t)y * w + x) * 4;
                const float r = p[0] / 255.0f, g = p[1] / 255.0f, b = p[2] / 255.0f, mx = std::max(r, std::max(g, b)),
                            mn = std::min(r, std::min(g, b)), d = mx - mn;
                if (p[3] <= 127 || d <= 1e-6f || d / mx <= sp.sat || mx <= sp.val) continue;
                float hue = mx == r ? std::fmod((g - b) / d + 6.0f, 6.0f) : mx == g ? (b - r) / d + 2 : (r - g) / d + 4;
                hue *= 60;
                if (hue < 18) hue += 360;   // (the ruby's window goes through 0)
                if (hue > sp.h0 && hue < sp.h1) m[(size_t)y * w + x] = 255;
            }
        spread(m, w, h, 2, true), spread(m, w, h, 2, false);
        // (the outside flooded from a corner, through its four neighbours: what it does not reach is the stone and its holes)
        std::vector<unsigned char> out((size_t)w * h, 0);
        std::vector<int> stack{0};
        out[0] = 1;
        while (!stack.empty()) {
            const int at = stack.back(), x = at % w, y = at / w;
            stack.pop_back();
            const int next[4] = {x > 0 ? at - 1 : -1, x + 1 < w ? at + 1 : -1, y > 0 ? at - w : -1, y + 1 < h ? at + w : -1};
            for (int n : next)
                if (n >= 0 && !out[n] && !m[n]) out[n] = 1, stack.push_back(n);
        }
        for (size_t i = 0; i < m.size(); i++) m[i] = out[i] ? 0 : 255;
        spread(m, w, h, 1, true);
        // (feathered: a Gaussian of 1.2 pixels)
        float kern[9], sum = 0;
        for (int i = -4; i <= 4; i++) sum += kern[i + 4] = std::exp(-(float)(i * i) / (2 * 1.2f * 1.2f));
        std::vector<float> a((size_t)w * h), t((size_t)w * h);
        for (size_t i = 0; i < m.size(); i++) a[i] = m[i] / 255.0f;
        for (int pass = 0; pass < 2; pass++) {
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    float v = 0;
                    for (int i = -4; i <= 4; i++) {
                        const int xx = std::clamp(pass ? x : x + i, 0, w - 1), yy = std::clamp(pass ? y + i : y, 0, h - 1);
                        v += a[(size_t)yy * w + xx] * kern[i + 4];
                    }
                    t[(size_t)y * w + x] = v / sum;
                }
            a.swap(t);
        }
        mask[k] = std::move(a);
    }
}
/// The layers from the crest's pixels: dim (the stones drained to a cold dark glass), each stone's own pixels, the shape
inline void crest_layers(const unsigned char* px, int w, int h, std::vector<unsigned char>& dim, std::vector<unsigned char> stone[3],
                         std::vector<unsigned char>& shape) {
    std::vector<float> mask[3];
    stone_masks(px, w, h, mask);
    const size_t n = (size_t)w * h;
    dim.assign(px, px + n * 4);
    shape.assign(n * 4, 255);
    for (size_t i = 0; i < n; i++) shape[i * 4 + 3] = px[i * 4 + 3];
    for (int k = 0; k < 3; k++) {
        stone[k].assign(px, px + n * 4);
        for (size_t i = 0; i < n; i++) {
            const float m = mask[k][i];
            stone[k][i * 4 + 3] = (unsigned char)(px[i * 4 + 3] * m + 0.5f);
            if (m <= 0) continue;
            const float lum = (0.299f * px[i * 4] + 0.587f * px[i * 4 + 1] + 0.114f * px[i * 4 + 2]) / 255.0f;
            const float unlit[3] = {lum * 0.30f + 0.020f, lum * 0.28f + 0.018f, lum * 0.26f + 0.016f};
            for (int ch = 0; ch < 3; ch++)
                dim[i * 4 + ch] = (unsigned char)std::clamp(dim[i * 4 + ch] * (1 - m) + unlit[ch] * 255.0f * m + 0.5f, 0.0f, 255.0f);
        }
    }
}
/// The textures, at the first drawing (ImGui's context exists then). Without the crest (a source tree that has no picture)
/// the screen is its type and its line alone
inline void art_make() {
    if (art.tried) return;
    art.tried = true;
    float aspect = 1;
    ImTextureData* crest = ui::logo(&aspect);
    const auto t0 = std::chrono::steady_clock::now();
    if (crest && crest->Format == ImTextureFormat_RGBA32 && crest->GetPixels() && crest->Width > 8 && crest->Height > 8) {
        std::vector<unsigned char> dim, stone[3], shape;
        art.w = crest->Width, art.h = crest->Height;
        crest_layers((const unsigned char*)crest->GetPixels(), art.w, art.h, dim, stone, shape);
        art.dim = texture_of(art.w, art.h, dim.data());
        for (int k = 0; k < 3; k++) art.stone[k] = texture_of(art.w, art.h, stone[k].data());
        art.shape = texture_of(art.w, art.h, shape.data());
    }
    std::vector<unsigned char> noise((size_t)256 * 256 * 4);
    uint32_t seed = 0x2F6E2B1u;
    for (size_t i = 0; i < noise.size(); i += 4) {
        seed = seed * 1664525u + 1013904223u;
        noise[i] = noise[i + 1] = noise[i + 2] = (unsigned char)(seed >> 24), noise[i + 3] = 255;
    }
    art.grain = texture_of(256, 256, noise.data());
    logf("vk: start-up: the crest's layers made in %.1f ms (%dx%d)", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(),
         art.w, art.h);
}
/// The textures let go, inside an ImGui frame that draws none of them: this frame's recording destroys the GPU's part
/// (vk_imgui.cpp, some frames later). WantDestroyNextFrame says the destroy is ours: without it ImGui takes a destroyed
/// texture for one the backend lost, and asks for it again at the next frame. The pixels go now; the small records stay
/// registered (ImGui walks them at every frame)
inline void art_release() {
    if (art.released) return;
    art.released = true;
    ImTextureData** all[] = {&art.dim, &art.stone[0], &art.stone[1], &art.stone[2], &art.shape, &art.grain};
    for (ImTextureData** t : all) {
        if (!*t) continue;
        const bool made = (*t)->BackendUserData || (*t)->TexID != ImTextureID_Invalid;   // (on the GPU; else there is nothing to destroy there)
        (*t)->WantDestroyNextFrame = true;
        (*t)->SetStatus(made ? ImTextureStatus_WantDestroy : ImTextureStatus_Destroyed);
        (*t)->DestroyPixels();
        *t = nullptr;
    }
}

// ---- drawing: meshes with a colour a vertex ----
/// A light round m: rings at rr (shares of the radii rx, ry, the first 0) with the alphas aa
inline void radial(ImDrawList* d, ImVec2 m, float rx, float ry, unsigned rgb, const float* rr, const float* aa, int n, float k = 1) {
    const int seg = 72;
    const ImVec2 uv = d->_Data->TexUvWhitePixel;
    d->PrimReserve((seg * 3) + (n - 2) * seg * 6, 1 + (n - 1) * seg);
    const unsigned base = d->_VtxCurrentIdx;
    d->PrimWriteVtx(m, uv, light(rgb, aa[0] * k));
    for (int i = 1; i < n; i++)
        for (int s = 0; s < seg; s++) {
            const float a = s * 6.283185307f / seg;
            d->PrimWriteVtx(ImVec2(m.x + std::cos(a) * rx * rr[i], m.y + std::sin(a) * ry * rr[i]), uv, light(rgb, aa[i] * k));
        }
    for (int s = 0; s < seg; s++) {
        const int s1 = (s + 1) % seg;
        d->PrimWriteIdx((ImDrawIdx)base), d->PrimWriteIdx((ImDrawIdx)(base + 1 + s)), d->PrimWriteIdx((ImDrawIdx)(base + 1 + s1));
    }
    for (int i = 1; i + 1 < n; i++)
        for (int s = 0; s < seg; s++) {
            const int s1 = (s + 1) % seg;
            const unsigned a = base + 1 + (i - 1) * seg + s, b = base + 1 + (i - 1) * seg + s1, c2 = base + 1 + i * seg + s1, e = base + 1 + i * seg + s;
            d->PrimWriteIdx((ImDrawIdx)a), d->PrimWriteIdx((ImDrawIdx)b), d->PrimWriteIdx((ImDrawIdx)c2);
            d->PrimWriteIdx((ImDrawIdx)a), d->PrimWriteIdx((ImDrawIdx)c2), d->PrimWriteIdx((ImDrawIdx)e);
        }
}
/// A soft light: alpha a at the middle, falling to nothing at the radii
inline void glow(ImDrawList* d, ImVec2 m, float rx, float ry, unsigned rgb, float a) {
    static const float rr[8] = {0, 0.12f, 0.25f, 0.4f, 0.55f, 0.7f, 0.85f, 1};
    float aa[8];
    for (int i = 0; i < 8; i++) aa[i] = a * (1 - rr[i]) * (1 - rr[i]) * (1 + 0.5f * rr[i]);
    radial(d, m, rx, ry, rgb, rr, aa, 8);
}
/// A grid over a rectangle, alpha(x, y) asked at each corner (x and y from 0 to 1): the vignette, the gleam
template <class F>
inline void grid(ImDrawList* d, ImVec2 a, ImVec2 b, int nx, int ny, unsigned rgb, bool textured, F alpha) {
    const ImVec2 white = d->_Data->TexUvWhitePixel;
    d->PrimReserve(nx * ny * 6, (nx + 1) * (ny + 1));
    const unsigned base = d->_VtxCurrentIdx;
    for (int y = 0; y <= ny; y++)
        for (int x = 0; x <= nx; x++) {
            const float fx = (float)x / nx, fy = (float)y / ny;
            d->PrimWriteVtx(ImVec2(lerp(a.x, b.x, fx), lerp(a.y, b.y, fy)), textured ? ImVec2(fx, fy) : white, ink(rgb, alpha(fx, fy)));
        }
    for (int y = 0; y < ny; y++)
        for (int x = 0; x < nx; x++) {
            const unsigned i = base + y * (nx + 1) + x;
            d->PrimWriteIdx((ImDrawIdx)i), d->PrimWriteIdx((ImDrawIdx)(i + 1)), d->PrimWriteIdx((ImDrawIdx)(i + nx + 2));
            d->PrimWriteIdx((ImDrawIdx)i), d->PrimWriteIdx((ImDrawIdx)(i + nx + 2)), d->PrimWriteIdx((ImDrawIdx)(i + nx + 1));
        }
}
/// A horizontal piece of light on a line: nothing at x0 and x1, a at the middle
inline void streak(ImDrawList* d, float x0, float x1, float y0, float y1, unsigned rgb, float a) {
    const float m = (x0 + x1) * 0.5f;
    const ImU32 on = light(rgb, a), off = light(rgb, 0);
    d->AddRectFilledMultiColor(ImVec2(x0, y0), ImVec2(m, y1), off, on, on, off);
    d->AddRectFilledMultiColor(ImVec2(m, y0), ImVec2(x1, y1), on, off, off, on);
}

// ---- type: letters set one by one, so they can be spaced, their digits kept in equal slots, and shaded down their height ----
struct Shade {   // (stops down the letters, from y0 to y1)
    float y0 = 0, y1 = 1;
    int n = 0;
    float at[4] = {};
    unsigned rgb[4] = {};
    ImU32 colour(float y, int alpha) const {
        const float t = clamp01((y - y0) / (y1 - y0));
        int i = 0;
        while (i + 2 < n && t > at[i + 1]) i++;
        const float k = clamp01((t - at[i]) / std::max(1e-6f, at[i + 1] - at[i]));
        const int r = (int)lerp((float)(rgb[i] >> 16 & 255), (float)(rgb[i + 1] >> 16 & 255), k),
                  g = (int)lerp((float)(rgb[i] >> 8 & 255), (float)(rgb[i + 1] >> 8 & 255), k),
                  b = (int)lerp((float)(rgb[i] & 255), (float)(rgb[i + 1] & 255), k);
        return IM_COL32(r, g, b, alpha);
    }
};
/// text at p (its top left); its width. track: pixels after each letter; slots: every digit as wide as the widest, centred in
/// its slot (a count that changes does not shake); draw false: measured only; shade: its colours, col's alpha
inline float type(ImDrawList* d, ImFont* font, float px, ImVec2 p, const char* text, ImU32 col, float track = 0, bool slots = false,
                  bool draw = true, const Shade* shade = nullptr) {
    ImFontBaked* baked = font->GetFontBaked(px);
    const float scale = px / baked->Size;
    const char* end = text + strlen(text);
    float slot = 0;
    if (slots)
        for (ImWchar ch = '0'; ch <= '9'; ch++) slot = std::max(slot, baked->FindGlyph(ch)->AdvanceX * scale);
    int count = 0;
    for (const char* s = text; s < end;) {   // (every glyph baked first: one baked later may move the earlier ones in the atlas)
        unsigned int ch = (unsigned char)*s;
        s += ch < 0x80 ? 1 : ImTextCharFromUtf8(&ch, s, end);
        baked->FindGlyph((ImWchar)ch);
        count++;
    }
    const float x0 = std::floor(p.x), y = std::floor(p.y);
    float x = x0;
    for (const char* s = text; s < end;) {
        unsigned int ch = (unsigned char)*s;
        s += ch < 0x80 ? 1 : ImTextCharFromUtf8(&ch, s, end);
        const ImFontGlyph* g = baked->FindGlyph((ImWchar)ch);
        float adv = g->AdvanceX * scale, gx = x;
        if (slots && ch >= '0' && ch <= '9') gx = std::floor(x + (slot - adv) * 0.5f + 0.5f), adv = slot;
        if (draw && g->Visible) {
            const float ax = gx + g->X0 * scale, ay = y + g->Y0 * scale, bx = gx + g->X1 * scale, by = y + g->Y1 * scale;
            if (!shade) {
                d->PrimReserve(6, 4);
                d->PrimRectUV(ImVec2(ax, ay), ImVec2(bx, by), ImVec2(g->U0, g->V0), ImVec2(g->U1, g->V1), col);
            } else {   // (cut at each stop inside the glyph: a quad has one colour a corner)
                float cut[8];
                int n = 0;
                cut[n++] = ay;
                for (int i = 0; i < shade->n; i++) {
                    const float sy = lerp(shade->y0, shade->y1, shade->at[i]);
                    if (sy > ay + 0.01f && sy < by - 0.01f) cut[n++] = sy;
                }
                cut[n++] = by;
                const int alpha = (int)(col >> IM_COL32_A_SHIFT & 255);
                for (int i = 0; i + 1 < n; i++) {
                    const float va = lerp(g->V0, g->V1, (cut[i] - ay) / (by - ay)), vb = lerp(g->V0, g->V1, (cut[i + 1] - ay) / (by - ay));
                    const ImU32 ca = shade->colour(cut[i], alpha), cb = shade->colour(cut[i + 1], alpha);
                    d->PrimReserve(6, 4);
                    const unsigned base = d->_VtxCurrentIdx;
                    d->PrimWriteIdx((ImDrawIdx)base), d->PrimWriteIdx((ImDrawIdx)(base + 1)), d->PrimWriteIdx((ImDrawIdx)(base + 2));
                    d->PrimWriteIdx((ImDrawIdx)base), d->PrimWriteIdx((ImDrawIdx)(base + 2)), d->PrimWriteIdx((ImDrawIdx)(base + 3));
                    d->PrimWriteVtx(ImVec2(ax, cut[i]), ImVec2(g->U0, va), ca), d->PrimWriteVtx(ImVec2(bx, cut[i]), ImVec2(g->U1, va), ca);
                    d->PrimWriteVtx(ImVec2(bx, cut[i + 1]), ImVec2(g->U1, vb), cb), d->PrimWriteVtx(ImVec2(ax, cut[i + 1]), ImVec2(g->U0, vb), cb);
                }
            }
        }
        x += adv + track;
    }
    return count ? x - x0 - track : 0;
}
/// The top to draw at for a baseline at y
inline float top_of(ImFont* font, float px, float baseline) { return baseline - font->GetFontBaked(px)->Ascent * (px / font->GetFontBaked(px)->Size); }

// ---- what moves: kept from one drawing to the next (the presenting thread alone draws) ----
struct Motion {
    double first = -1, last = 0;
    double info_at = -1;        // (when the lower part first showed)
    std::string title;          // (the step shown: a new one fades in, its line from nothing)
    double title_at = -1;
    float fill = 0;
    bool lit[3] = {};
    double ignite[3] = {-1e9, -1e9, -1e9}, last_ignite = -1e9;
    float kindle[3] = {};
    double gleam_at = -1e9;
    bool gleamed_open = false, gleamed_lit = false;
};
inline Motion motion;

/// A block arriving at `at`: its alpha, and in *rise how far under its place it still is (12 points, 320 ms)
inline float arrive(double now, double at, float u, float* rise) {
    const float t = ease_out((float)((now - at) / 0.32));
    if (rise) *rise = (1 - t) * 12 * u;
    return now < at ? 0 : t;
}

/// The whole screen for view v at time now (seconds, any steady clock); fade, 0 to 1, takes it to black as it ends
inline void draw(ui::Ctx& c, const StartupView& v, float fade, double now) {
    ImDrawList* d = c.d;
    const float w = c.w, h = c.h;
    Motion& m = motion;
    if (fade >= 1.5f) art_release();   // (vk_main.cpp: the last picture)
    if (art.released) {   // (the game takes over: black, nothing of ours)
        d->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), hex(0));
        return;
    }
    art_make();
    if (m.first < 0) m.first = m.last = now;
    const float dt = (float)std::clamp(now - m.last, 0.0, 0.25);
    m.last = now;
    blend_power = v.linear ? 2.2f : 1.0f;

    // (the tokens' pixel: of a 640-line window, the overlay's own size setting kept; smaller where the window is too
    // narrow or too low for the column)
    float u = c.s * 1.125f;
    u = std::min(u, std::min((w - 2 * c.safe) / 430.0f, (h - 2 * c.safe) / 484.0f));
    const float cx = std::floor(w * 0.5f), hair = std::max(1.0f, std::floor(u + 0.5f));
    const float crest_h = 180, stack = (crest_h + 210) * u, top = std::floor(std::max(c.safe, (h - stack) * 0.5f - 18 * u));
    // (the crest: 28% of the window's height at the usual size, never above its own pixels)
    float ch = crest_h * u, cw = art.h ? ch * art.w / art.h : ch * 1.094f;
    if (art.w && cw > art.w) cw = (float)art.w, ch = (float)art.h;
    const ImVec2 ca(std::floor(cx - cw * 0.5f), std::floor(top + (crest_h * u - ch) * 0.5f)), cb(ca.x + cw, ca.y + ch), cm((ca.x + cb.x) * 0.5f, (ca.y + cb.y) * 0.5f);
    const float in0 = arrive(now, m.first, u, nullptr);   // (the hall and the crest as the window opens)

    // ---- the hall ----
    d->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), hex(kInk900));
    d->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(w, h * 0.45f), light(kTopLight, 0.045f), light(kTopLight, 0.045f), light(kTopLight, 0),
                               light(kTopLight, 0));
    {   // (the halo behind the crest, breathing)
        static const float rr[7] = {0, 0.15f, 0.30f, 0.45f, 0.62f, 0.80f, 1};
        static const float aa[7] = {0.22f, 0.17f, 0.12f, 0.07f, 0.046f, 0.022f, 0};
        const float k = in0 * breath(now, 6.0f, 0.80f) * (v.closing ? 0.6f : 1.0f);
        radial(d, cm, cw * 1.15f, cw * 1.15f, kGold500, rr, aa, 7, k);
    }
    {   // (the vignette: nothing until 55% of the way to a corner, then to 0.55 black)
        const float half = std::sqrt(w * w + h * h) * 0.5f;
        grid(d, ImVec2(0, 0), ImVec2(w, h), 32, 18, 0x000000, false, [&](float fx, float fy) {
            const float dx = (fx - 0.5f) * w, dy = (fy - 0.5f) * h, t = clamp01((std::sqrt(dx * dx + dy * dy) / half - 0.55f) / 0.45f);
            return 0.55f * t * t * (3 - 2 * t);
        });
    }
    if (art.grain) {   // (grain, a texel a pixel)
        d->PushTexture(art.grain->GetTexRef());
        const ImU32 col = light(0xFFFFFF, 0.025f);
        for (float y = 0; y < h; y += 256)
            for (float x = 0; x < w; x += 256) {
                const float x1 = std::min(x + 256, w), y1 = std::min(y + 256, h);
                d->PrimReserve(6, 4);
                d->PrimRectUV(ImVec2(x, y), ImVec2(x1, y1), ImVec2(0, 0), ImVec2((x1 - x) / 256, (y1 - y) / 256), col);
            }
        d->PopTexture();
    }
    for (int i = 0; i < 26; i++) {   // (motes: specks of light rising through the stage, born and fading inside it)
        uint32_t s = (uint32_t)i * 2654435761u + 0x9E3779B9u;
        const auto rnd = [&s] { s ^= s << 13, s ^= s >> 17, s ^= s << 5; return (s & 0xFFFFFF) / 16777216.0f; };
        const float life = lerp(9, 17, rnd()), phase = rnd(), x0 = lerp(-1.25f, 1.25f, rnd()), y0 = lerp(-0.25f, 0.75f, rnd()), speed = lerp(6, 14, rnd()),
                    size = lerp(1.0f, 2.5f, rnd()), alpha = lerp(0.10f, 0.45f, rnd()), sway = lerp(0.3f, 0.7f, rnd());
        const float t = (float)std::fmod(now / life + phase, 1.0), age = t * life;
        const ImVec2 p(cm.x + x0 * cw + std::sin((float)now * sway + phase * 6.28f) * 7 * u, cm.y + y0 * ch + 0.5f * ch - age * speed * u);
        const float a = alpha * std::sin(t * 3.14159265f) * in0 * (v.closing ? 0.4f : 1.0f);
        d->AddCircleFilled(p, std::max(0.8f, size * u * 0.5f), light(kGold100, a), 8);
    }

    // ---- the crest and its stones ----
    glow(d, ImVec2(cm.x, cb.y + 5 * u), cw * 0.46f, ch * 0.075f, kGold500, 0.18f * in0);   // (the light on the floor under it)
    const float gone = v.closing ? 0.55f : 1.0f;   // (closing: the light goes down)
    if (art.dim) {
        d->AddImage(art.dim->GetTexRef(), ca, cb, ImVec2(0, 0), ImVec2(1, 1), ink(0xFFFFFF, in0));
        float done_a[3] = {};
        for (int k = 0; k < 3; k++) {
            // (the last one a little before the end, 99% of the last part: lit, it is seen before the screen fades)
            const bool want = !v.closing && (v.done || v.stones >= (k == 2 ? 2.93 : k + 1.0));
            if (want && !m.lit[k]) {
                m.lit[k] = true;
                // (lit as the screen opens, a start with nothing to prepare: it is so, and comes out of the dark with the crest;
                // nothing ignites. Such a start can be over in 0.7 s (measured): three stones igniting one after the other were
                // cut off at the second. Later: not before the crest is there, and two at once one after the other)
                if (now - m.first < 0.12) m.ignite[k] = now - 10;
                else m.last_ignite = m.ignite[k] = std::max(std::max(now, m.first + 0.35), m.last_ignite + 0.22);
            }
            const float target = clamp01((float)v.stones - k) * 0.30f;
            m.kindle[k] += (target - m.kindle[k]) * (1 - std::exp(-dt / 0.35f));
            const float since = (float)(now - m.ignite[k]);
            const float on = m.lit[k] && since >= 0 ? ease_out(since / 0.6f) : 0;
            const float a = std::max(m.kindle[k] * breath(now, 3.2f, 0.72f, k / 3.0f), on);
            done_a[k] = on;
            if (a > 0.004f && art.stone[k]) d->AddImage(art.stone[k]->GetTexRef(), ca, cb, ImVec2(0, 0), ImVec2(1, 1), ink(0xFFFFFF, a * in0 * gone));
        }
        for (int k = 0; k < 3; k++) {   // (the light the stones throw: over the crest)
            const ImVec2 at(lerp(ca.x, cb.x, kStoneAt[k][0]), lerp(ca.y, cb.y, kStoneAt[k][1]));
            const float since = (float)(now - m.ignite[k]);
            if (done_a[k] > 0)
                glow(d, at, cw * 0.22f, cw * 0.22f, kStoneGlow[k], 0.16f * done_a[k] * breath(now, 4.5f, 0.6f, k / 3.0f) * in0 * gone);
            else if (m.kindle[k] > 0.004f)
                glow(d, at, cw * 0.16f, cw * 0.16f, kStoneGlow[k], 0.10f * (m.kindle[k] / 0.30f) * breath(now, 3.2f, 0.72f, k / 3.0f) * in0 * gone);
            if (m.lit[k] && since >= 0 && since < 0.9f) {   // (the bloom as it ignites)
                const float t = since / 0.9f, r = cw * lerp(0.45f, 0.90f, ease_out(t));
                glow(d, at, r, r, kStoneGlow[k], 0.55f * (1 - t) * (1 - t) * in0);
            }
        }
        // (the gleam: once when all three are lit; and over the dark crest of a start that has work to do, a second in)
        if (!m.gleamed_open && now - m.first > 1.2) {
            m.gleamed_open = true;
            if (!m.lit[0]) m.gleam_at = now;
        }
        if (m.lit[0] && m.lit[1] && m.lit[2] && !m.gleamed_lit)
            m.gleamed_lit = true, m.gleam_at = std::max(std::max(now, m.ignite[2] + 0.35), m.first + 0.45);
        const float gt = (float)((now - m.gleam_at) / 1.1);
        if (gt > 0 && gt < 1 && art.shape && !v.closing) {
            const float slant = 0.577f * ch / cw, half = 0.11f, mid = lerp(-half - slant * 0.5f, 1 + half + slant * 0.5f, ease_io(gt));
            d->PushTexture(art.shape->GetTexRef());
            grid(d, ca, cb, 40, 12, kGold100, true, [&](float fx, float fy) {
                const float q = (fx + (0.5f - fy) * slant - mid) / half;
                return q <= -1 || q >= 1 ? 0.0f : 0.35f * (1 - q * q) * (1 - q * q) * in0 * (blend_power != 1 ? 0.45f : 1.0f);
            });
            d->PopTexture();
        }
    }

    // ---- the wordmark ----
    {
        float rise = 0;
        const float a = arrive(now, m.first + 0.045, u, &rise), px = 25 * u, track = 0.40f * px;
        const float tw = type(d, c.f.heading, px, ImVec2(), "TRIFORCE", 0, track, false, false);
        const float base = top + (crest_h + 22) * u + 0.74f * px + rise;
        Shade sh;
        sh.y0 = base - 0.72f * px, sh.y1 = base + 0.02f * px, sh.n = 4;
        const float at[4] = {0, 0.42f, 0.56f, 1};
        const unsigned rgb[4] = {0xFFF1B8, 0xE8B840, 0xB57E1C, 0xF3CF6A};
        memcpy(sh.at, at, sizeof at), memcpy(sh.rgb, rgb, sizeof rgb);
        type(d, c.f.heading, px, ImVec2(cx - tw * 0.5f, top_of(c.f.heading, px, base)), "TRIFORCE", ink(0xFFFFFF, a * (v.closing ? 0.7f : 1.0f)), track, false,
             true, &sh);
    }

    // ---- the lower part: the step, how far, the line, what it is ----
    const float col_w = std::floor(std::min(w - 2 * c.safe - 8 * u, std::max(w * 0.38f, 420 * u)));
    const float x0 = std::floor(cx - col_w * 0.5f), x1 = x0 + col_w;
    if (!v.title.empty()) {
        if (m.info_at < 0) m.info_at = now;
        // (a new step, or the same step's next pass (the noted pipelines after the predicted ones: the same title, another
        // sentence, the count from 0 again): it fades in, and its line starts from nothing. Seen in the game without this: the
        // line ran backwards from full under "0%")
        const std::string step = v.title + "\n" + v.detail;
        if (m.title != step) {
            m.title = step, m.title_at = now;
            m.fill = v.done ? m.fill : 0;
        }
        const float change = m.title_at > m.info_at ? ease_out((float)((now - m.title_at) / 0.22)) : 1;
        float rise[4];
        float a[4];
        for (int i = 0; i < 4; i++) a[i] = arrive(now, m.info_at + 0.045 * i, u, &rise[i]);
        const float y_eye = top + (crest_h + 102) * u, y_num = top + (crest_h + 168) * u, y_line = std::floor(top + (crest_h + 184) * u), y_text = y_line + 13 * u;
        {   // (the eyebrow: a hairline, then the step in spaced capitals)
            std::string label = v.step.empty() ? "" : v.step + "  \xC2\xB7  ";
            for (char ch2 : v.title) label += (char)toupper((unsigned char)ch2);
            const float px = 10.5f * u, y = y_eye + rise[0], aa = a[0] * change;
            d->AddRectFilled(ImVec2(x0, std::floor(y)), ImVec2(x0 + 20 * u, std::floor(y) + hair), ink(kGold500, 0.28f * 2.0f * aa));
            type(d, c.f.strong, px, ImVec2(x0 + 30 * u, ui::mid_top(c.f.strong, px, y + hair * 0.5f)), label.c_str(), ink(kGold500, aa), 0.22f * px);
        }
        const float base = y_num + rise[1];
        if (!v.pct.empty()) {   // (the percentage: the figures large, its sign small and gold)
            std::string digits = v.pct;
            if (!digits.empty() && digits.back() == '%') digits.pop_back();
            const float px = 64 * u, aa = a[1] * change;
            const float nw = type(d, c.f.heading, px, ImVec2(x0 - 2 * u, top_of(c.f.heading, px, base)), digits.c_str(), ink(kTextHi, aa), 0, true);
            type(d, c.f.heading, 24 * u, ImVec2(x0 - 2 * u + nw + 5 * u, top_of(c.f.heading, 24 * u, base)), "%", ink(kGold500, aa));
        }
        if (v.pct.empty() && !v.left.empty()) {   // (nothing counted: what is being done, in the figures' place, a quiet title)
            const float px = 30 * u;
            type(d, c.f.heading, px, ImVec2(x0, top_of(c.f.heading, px, base - 6 * u)), v.left.c_str(), ink(kTextHi, a[1] * change));
        } else {   // (the detail, set to the right: the count over the time left, or one of them on the figures' line)
            const float px = 12.5f * u, aa = a[1] * change;
            const bool two = !v.left.empty() && !v.right.empty();
            const auto right = [&](const std::string& t, float baseline, unsigned rgb) {
                if (t.empty()) return;
                const float tw = type(d, c.f.body, px, ImVec2(), t.c_str(), 0, 0, true, false);
                type(d, c.f.body, px, ImVec2(x1 - tw, top_of(c.f.body, px, baseline)), t.c_str(), ink(rgb, aa), 0, true);
            };
            right(v.left, two ? base - 19 * u : base, kText);
            right(v.right, base, kTextDim);
        }
        {   // (the line)
            const float y = y_line + rise[2], aa = a[2], th = std::max(2.0f, std::floor(2 * u + 0.5f));
            d->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + th), ink(kInk600, aa));
            if (v.fraction >= 0) {
                const float target = (float)std::min(v.fraction, 1.0);
                m.fill += (target - m.fill) * (1 - std::exp(-dt / 0.10f));   // (never a jump: there in 300 ms)
                if (std::fabs(target - m.fill) < 0.0004f) m.fill = target;
                const float fx = x0 + col_w * m.fill, fa = aa * change;
                if (fx > x0 + 0.5f) {
                    const float gl = 8 * u;   // (the glow under the fill)
                    d->AddRectFilledMultiColor(ImVec2(x0, y - gl), ImVec2(fx, y + th * 0.5f), light(kGold500, 0), light(kGold500, 0), light(kGold500, 0.30f * fa),
                                               light(kGold500, 0.30f * fa));
                    d->AddRectFilledMultiColor(ImVec2(x0, y + th * 0.5f), ImVec2(fx, y + th + gl), light(kGold500, 0.30f * fa), light(kGold500, 0.30f * fa),
                                               light(kGold500, 0), light(kGold500, 0));
                    d->AddRectFilledMultiColor(ImVec2(x0, y), ImVec2(fx, y + th), ink(kGold500, fa), ink(kGold300, fa), ink(kGold300, fa), ink(kGold500, fa));
                    // (a glint along what is filled, every 2.4 s: the screen is alive while a count stands still)
                    const float gt = (float)std::fmod(now, 2.4) / 1.5f, len = 70 * u;
                    if (gt < 1 && fx - x0 > len * 0.6f && !v.done) {
                        const float gx = lerp(x0 - len, fx, ease_io(gt));
                        d->PushClipRect(ImVec2(x0, y - 2 * u), ImVec2(fx, y + th + 2 * u), true);
                        streak(d, gx, gx + len, y, y + th, kGold100, 0.9f * fa);
                        d->PopClipRect();
                    }
                    // (the spark at its head: a small diamond in its own light)
                    glow(d, ImVec2(fx, y + th * 0.5f), 11 * u, 11 * u, kGold300, 0.50f * fa);
                    ui::diamond(c, ImVec2(fx, y + th * 0.5f), 3.5f * u, ink(kGold100, fa));
                }
            } else if (v.busy || v.closing) {   // (no count: a piece of the line goes across, 1.6 s each time)
                const float t = ease_io((float)std::fmod(now, 1.6) / 1.6f), len = col_w * 0.28f, sx = lerp(x0 - len, x1, t);
                d->PushClipRect(ImVec2(x0, y - 9 * u), ImVec2(x1, y + th + 9 * u), true);
                streak(d, sx, sx + len, y, y + th, kGold500, (v.closing ? 0.55f : 1.0f) * aa);
                d->PopClipRect();
            }
        }
        if (!v.detail.empty())
            ui::para(c, c.f.body, 12.5f * u, ImVec2(x0, y_text + rise[3]), ink(kTextDim, a[3] * change), v.detail.c_str(), col_w);
    }

    // ---- the foot: one line of what to know, the build and the graphics card in the corners ----
    {
        const float px = 12.5f * u, y = h - c.safe - px - 6 * u, a = arrive(now, m.first + 0.2, u, nullptr);
        if (!v.hint.empty() && m.info_at >= 0) {
            const float ha = arrive(now, m.info_at + 0.25, u, nullptr);
            const float tw = ui::size_of(c.f.body, px, v.hint.c_str()).x;
            if (tw <= w - 2 * c.safe) ui::text(c, c.f.body, px, ImVec2(std::floor(cx - tw * 0.5f), y), ink(kTextDim, ha), v.hint.c_str());
            else ui::para(c, c.f.body, px, ImVec2(c.safe, y - px * 1.3f), ink(kTextDim, ha), v.hint.c_str(), w - 2 * c.safe);
        }
        // (the corners on the hint's line while both have room beside it, else a line above it)
        const float side = c.safe + 6 * u, hint_w = v.hint.empty() || m.info_at < 0 ? 0 : ui::size_of(c.f.body, px, v.hint.c_str()).x;
        const float corner_w = std::max(ui::size_of(c.f.body, px, v.version.c_str()).x, ui::size_of(c.f.body, px, v.device.c_str()).x);
        const float yy = hint_w * 0.5f + 40 * u + corner_w + side < w * 0.5f ? y : y - 24 * u;
        if (!v.version.empty()) ui::text(c, c.f.body, px, ImVec2(side, yy), ink(kTextFaint, a), v.version.c_str());
        if (!v.device.empty()) {
            const float tw = ui::size_of(c.f.body, px, v.device.c_str()).x;
            if (tw < w * 0.5f - side) ui::text(c, c.f.body, px, ImVec2(w - side - tw, yy), ink(kTextFaint, a), v.device.c_str());
        }
    }

    if (fade > 0) d->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), ink(0x000000, fade));   // (the end: to black, the game's first frame next)
}

/// The clock draw goes by in the game
inline double clock_now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace mkwvk::start_screen
