// (triforce) The start-up screen drawn with no game, no window and no GPU: gpu/vk/vk_startup_screen.h's own drawing through
// Dear ImGui's draw lists into a picture, by a small software rasterizer (triangles with a colour and a texture coordinate a
// corner, blended as the window's 8-bit image blends them). tests/startup_screen_preview.py builds and runs it: the previews
// of every state of the screen, and tests/test_startup_screen.py's checks.
//   startup_screen_preview <out folder> <romfs folder or -> <fonts folder> [--only name] [--size WxH] [--before]
// The fonts are the game's own, read from the player's romfs in memory as the game does (never written anywhere), else
// Libertinus from the fonts folder. Each picture is written as <name>.rgba: width, height (32-bit), then RGBA bytes.
#define IMGUI_DEFINE_MATH_OPERATORS
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"
#include "vk_game_font.h"
#include "vk_progress.h"
#include "vk_startup_screen.h"
#if __has_include("hookshot_logo.h")
#include "hookshot_logo.h"
#endif

using namespace mkwvk;

// ---- what vk_imgui.cpp gives the drawing in the game ----
static ImTextureData* crest = nullptr;
static float crest_aspect = 1;
namespace mkwvk {
void logf(const char*, ...) {}
}
namespace mkwvk::ui {
ImTextureData* logo(float* aspect) {
    *aspect = crest_aspect;
    return crest;
}
}

// ---- the rasterizer ----
struct Picture {
    int w = 0, h = 0;
    std::vector<unsigned char> px;   // (RGBA, alpha 255)
};
struct Texel { float r, g, b, a; };
static Texel sample(const ImTextureData* t, float u, float v) {
    if (!t || !t->Pixels) return {1, 1, 1, 1};
    const float x = u * t->Width - 0.5f, y = v * t->Height - 0.5f;
    const int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
    const float fx = x - x0, fy = y - y0;
    const auto at = [&](int xx, int yy) -> Texel {
        xx = xx < 0 ? 0 : xx >= t->Width ? t->Width - 1 : xx, yy = yy < 0 ? 0 : yy >= t->Height ? t->Height - 1 : yy;
        const unsigned char* p = t->Pixels + ((size_t)yy * t->Width + xx) * t->BytesPerPixel;
        if (t->BytesPerPixel == 1) return {1, 1, 1, p[0] / 255.0f};
        return {p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f};
    };
    const Texel a = at(x0, y0), b = at(x0 + 1, y0), c = at(x0, y0 + 1), e = at(x0 + 1, y0 + 1);
    const auto mix = [&](float p, float q, float r, float s) { return (p * (1 - fx) + q * fx) * (1 - fy) + (r * (1 - fx) + s * fx) * fy; };
    return {mix(a.r, b.r, c.r, e.r), mix(a.g, b.g, c.g, e.g), mix(a.b, b.b, c.b, e.b), mix(a.a, b.a, c.a, e.a)};
}
static void triangle(Picture& pic, const ImDrawVert& A, const ImDrawVert& B0, const ImDrawVert& C0, const ImTextureData* tex, const ImVec4& clip) {
    const ImDrawVert* b = &B0;
    const ImDrawVert* c = &C0;
    const auto fx = [](float v) { return (int64_t)std::llround((double)v * 256.0); };
    int64_t ax = fx(A.pos.x), ay = fx(A.pos.y), bx = fx(b->pos.x), by = fx(b->pos.y), cx = fx(c->pos.x), cy = fx(c->pos.y);
    int64_t area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (!area) return;
    if (area < 0) std::swap(b, c), std::swap(bx, cx), std::swap(by, cy), area = -area;
    const int x0 = std::max((int)std::floor(std::min({A.pos.x, b->pos.x, c->pos.x})), std::max(0, (int)clip.x)),
              x1 = std::min((int)std::ceil(std::max({A.pos.x, b->pos.x, c->pos.x})), std::min(pic.w, (int)clip.z)),
              y0 = std::max((int)std::floor(std::min({A.pos.y, b->pos.y, c->pos.y})), std::max(0, (int)clip.y)),
              y1 = std::min((int)std::ceil(std::max({A.pos.y, b->pos.y, c->pos.y})), std::min(pic.h, (int)clip.w));
    // (the top-left rule: a pixel on an edge two triangles share is drawn once)
    const auto bias = [](int64_t dx, int64_t dy) { return (dy == 0 && dx > 0) || dy < 0 ? 0 : -1; };
    const int64_t b_ab = bias(bx - ax, by - ay), b_bc = bias(cx - bx, cy - by), b_ca = bias(ax - cx, ay - cy);
    const auto col = [](ImU32 v, int shift) { return (float)(v >> shift & 255) / 255.0f; };
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            const int64_t px = (int64_t)x * 256 + 128, py = (int64_t)y * 256 + 128;
            const int64_t e_ab = (bx - ax) * (py - ay) - (by - ay) * (px - ax), e_bc = (cx - bx) * (py - by) - (cy - by) * (px - bx),
                          e_ca = (ax - cx) * (py - cy) - (ay - cy) * (px - cx);
            if (e_ab + b_ab < 0 || e_bc + b_bc < 0 || e_ca + b_ca < 0) continue;
            const float wa = (float)e_bc / area, wb = (float)e_ca / area, wc = (float)e_ab / area;
            const Texel t = sample(tex, A.uv.x * wa + b->uv.x * wb + c->uv.x * wc, A.uv.y * wa + b->uv.y * wb + c->uv.y * wc);
            float rgba[4];
            const int shifts[4] = {IM_COL32_R_SHIFT, IM_COL32_G_SHIFT, IM_COL32_B_SHIFT, IM_COL32_A_SHIFT};
            const float tv[4] = {t.r, t.g, t.b, t.a};
            for (int k = 0; k < 4; k++) rgba[k] = (col(A.col, shifts[k]) * wa + col(b->col, shifts[k]) * wb + col(c->col, shifts[k]) * wc) * tv[k];
            unsigned char* o = &pic.px[((size_t)y * pic.w + x) * 4];
            for (int k = 0; k < 3; k++) o[k] = (unsigned char)std::lround(std::clamp(rgba[k] * rgba[3] * 255.0f + o[k] * (1 - rgba[3]), 0.0f, 255.0f));
        }
}
/// The textures ImGui wants made or let go, as the game's backend answers them (their pixels are read where they are)
static void textures() {
    for (ImTextureData* t : ImGui::GetPlatformIO().Textures) {
        if (t->Status == ImTextureStatus_WantCreate || t->Status == ImTextureStatus_WantUpdates) {
            t->SetTexID((ImTextureID)(intptr_t)t);
            t->SetStatus(ImTextureStatus_OK);
        } else if (t->Status == ImTextureStatus_WantDestroy) {
            t->SetTexID(ImTextureID_Invalid);
            t->SetStatus(ImTextureStatus_Destroyed);
        }
    }
}
static size_t last_vertices = 0;
static void rasterize(Picture& pic, const ImDrawData* dd) {
    last_vertices = 0;
    for (const ImDrawList* list : dd->CmdLists) {
        last_vertices += (size_t)list->VtxBuffer.Size;
        for (const ImDrawCmd& cmd : list->CmdBuffer) {
            if (cmd.UserCallback) continue;
            const ImTextureData* tex = (const ImTextureData*)(intptr_t)cmd.GetTexID();
            const ImDrawIdx* idx = list->IdxBuffer.Data + cmd.IdxOffset;
            const ImDrawVert* vtx = list->VtxBuffer.Data + cmd.VtxOffset;
            for (unsigned i = 0; i + 2 < cmd.ElemCount; i += 3) triangle(pic, vtx[idx[i]], vtx[idx[i + 1]], vtx[idx[i + 2]], tex, cmd.ClipRect);
        }
    }
}
static bool write(const Picture& pic, const std::string& path) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    const uint32_t wh[2] = {(uint32_t)pic.w, (uint32_t)pic.h};
    fwrite(wh, 4, 2, f);
    fwrite(pic.px.data(), 1, pic.px.size(), f);
    fclose(f);
    return true;
}

// ---- the fonts, as vk_imgui.cpp loads them ----
static ui::Fonts fonts;
static bool load_fonts(const std::string& romfs, const std::string& bundled) {
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    const auto game = [&](game_font::Reader& r, const char* name) -> ImFont* {
        std::vector<unsigned char> bytes = r.read(name);
        if (bytes.empty()) return nullptr;
        void* font = IM_ALLOC(bytes.size());
        memcpy(font, bytes.data(), bytes.size());
        ImFontConfig cfg;
        snprintf(cfg.Name, sizeof cfg.Name, "%s", name);
        return atlas->AddFontFromMemoryTTF(font, (int)bytes.size(), 18.0f, &cfg);
    };
    static game_font::Reader reader;
    if (romfs != "-" && reader.open(romfs)) {
        fonts.body = game(reader, "ClarimoUDPE-Regular");
        fonts.strong = fonts.body ? game(reader, "ClarimoUDPE-DemiBold") : nullptr;
        fonts.heading = fonts.strong ? game(reader, "CetraDisplayPro-Medium") : nullptr;
        if (fonts.heading) return true;
        atlas->Clear();
    }
    fonts.body = atlas->AddFontFromFileTTF((bundled + "/LibertinusSans-Regular.ttf").c_str(), 18.0f);
    fonts.strong = atlas->AddFontFromFileTTF((bundled + "/LibertinusSans-Bold.ttf").c_str(), 18.0f);
    fonts.heading = atlas->AddFontFromFileTTF((bundled + "/LibertinusSerif-Semibold.ttf").c_str(), 18.0f);
    return false;
}

// ---- the screen as it was before the redesign (gpu/vk/vk_menu.cpp draw_startup and vk_progress.h startup_view at 2290d02),
// for the pictures that set the two side by side ----
struct OldView {
    std::string version, step, title, pct, left, right, hint;
    double fraction = -1;
};
static OldView old_view(const StartupState& s) {
    OldView v;
    if (s.stopping) {
        v.title = "Closing", v.left = "Keeping what is prepared for the next start";
        return v;
    }
    if (s.finished) {
        if (s.secs >= 1.0) v.title = "Starting the game";
        return v;
    }
    if (s.secs < 1.0) return v;
    const bool pipelines = s.stage > 0;
    if (s.prepared) v.step = pipelines ? "STEP 2 OF 2" : "STEP 1 OF 2";
    v.title = !pipelines ? "Preparing shaders" : s.prepared ? "Building pipelines" : "Loading pipelines";
    if (s.prepared) v.hint = "This happens once after an update. Closing the window keeps what is done.";
    if (!s.total) {
        v.left = !pipelines ? "Reading the game's shaders" : s.stage == 1 ? "Reading the pipeline notes" : "Loading the shader cache";
        return v;
    }
    v.fraction = (double)std::min(s.done, s.total) / (double)s.total;
    v.pct = percent_text(s.done, s.total);
    v.left = with_commas(s.done) + " / " + with_commas(s.total) + (pipelines ? " pipelines" : " shaders");
    v.right = left_text(s.left);
    return v;
}
static void draw_old(ui::Ctx& c, const OldView& v, float fade) {
    const float s = c.s, w = c.w, h = c.h, cx = w * 0.5f, k = s * 0.86f;
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
        if (!v.step.empty()) {
            std::string step = v.step;
            for (size_t i = 1; i < step.size(); i++) step[i] = (char)tolower((unsigned char)step[i]);
            ui::text_centre(c, c.f.body, 15 * k, ImVec2(cx, y), ui::kAccent, step.c_str());
        }
        ui::text_centre(c, c.f.heading, 24 * k, ImVec2(cx, y + 20 * k), ui::kInk, v.title.c_str());
        const float x0 = cx - 270 * k, x1 = cx + 270 * k, by = y + 64 * k;
        float under = by;
        if (v.fraction >= 0) {
            const float bh = 26 * k, in = 4 * k, fx = x0 + in + (x1 - x0 - 2 * in) * (float)std::min(v.fraction, 1.0);
            c.d->AddRectFilled(ImVec2(x0, by), ImVec2(x1, by + bh), ui::kCellDark);
            c.d->AddRect(ImVec2(x0, by), ImVec2(x1, by + bh), ui::kEdge, 0, std::max(1.0f, s));
            if (fx > x0 + in) ui::gold(c, ImVec2(x0 + in, by + in), ImVec2(fx, by + bh - in));
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
        ui::text(c, c.f.body, px, ImVec2(v.fraction >= 0 ? x0 : cx - ui::size_of(c.f.body, px, v.left.c_str()).x * 0.5f, under + 12 * k), ui::kSoft,
                 v.left.c_str());
        if (!v.right.empty()) ui::text(c, c.f.body, px, ImVec2(x1 - ui::size_of(c.f.body, px, v.right.c_str()).x, under + 12 * k), ui::kSoft, v.right.c_str());
        if (!v.hint.empty()) ui::text_centre(c, c.f.body, 15.5f * k, ImVec2(cx, by + 92 * k), ui::kDim, v.hint.c_str());
    }
    if (fade > 0) c.d->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), ui::rgba(0, 0, 0, (int)(std::min(fade, 1.0f) * 255 + 0.5f)));
}

// ---- a start played through: what the preparation reports as time passes (vk_main.cpp's painter, with no workers) ----
struct Step {
    int stage, pass;
    size_t total;
    double wait, secs;   // (seconds with no count yet, then seconds counting)
};
struct Story {
    std::vector<Step> steps;
    bool prepared = false, compile = false;   // (compile: the driver's part of the shaders is on, vk_draw.cpp prepare_compiles)
    double close_at = -1;   // (the window closed then)
};
struct Moment {
    StartupState st;
    float fade = 0;
};
static Moment moment_of(const Story& story, double t, ProgressClock& eta, int& eta_key) {
    Moment m;
    m.st.secs = t, m.st.prepared = false, m.st.compile = story.compile;
    double at = 0;
    bool found = false;
    for (size_t i = 0; i < story.steps.size(); i++) {
        const Step& s = story.steps[i];
        if (s.stage == 0) m.st.prepared = true;
        if (found) continue;
        if (t < at + s.wait + s.secs || (story.close_at >= 0 && story.close_at < at + s.wait + s.secs)) {
            const double tt = story.close_at >= 0 ? std::min(t, story.close_at) : t;
            m.st.stage = s.stage, m.st.pass = s.pass;
            if (tt >= at + s.wait) {
                m.st.total = s.total;
                // (the programs differ: a slow stretch in the middle, as a few heavy ones make)
                double f = (tt - at - s.wait) / s.secs;
                f = f < 0.4 ? f * 0.9 : f < 0.6 ? 0.36 + (f - 0.4) * 0.6 : 0.48 + (f - 0.6) * 1.3;
                m.st.done = (size_t)(std::clamp(f, 0.0, 1.0) * s.total);
            }
            found = true;
        }
        at += s.wait + s.secs;
    }
    if (story.close_at >= 0 && t >= story.close_at) m.st.stopping = true;
    if (!found) {
        m.st.finished = true, m.st.stage = 2;
        m.fade = (float)std::clamp((t - at - 0.35) / 0.4, 0.0, 1.0);   // (held a moment here so its last picture can be taken)
    }
    const int key = m.st.stage * 2 + m.st.pass;
    if (key != eta_key) eta.reset(), eta_key = key;
    if (m.st.total) eta.note(t, m.st.done, m.st.total);
    m.st.left = m.st.total ? eta.left() : -1;
    return m;
}

struct Shot {
    const char* name;
    const Story* story;
    double at;
};

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: startup_screen_preview <out folder> <romfs folder or -> <fonts folder> [--only name] [--size WxH] [--before] [--large] [--linear]\n");
        return 2;
    }
    const std::string out = argv[1], romfs = argv[2], bundled = argv[3];
    std::string only;
    int W = 1920, H = 1080;
    bool before = false, large = false, linear = false, no_crest = false, frames = false;
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--only") && i + 1 < argc) only = argv[++i];
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &W, &H);
        else if (!strcmp(argv[i], "--before")) before = true;
        else if (!strcmp(argv[i], "--large")) large = true;
        else if (!strcmp(argv[i], "--linear")) linear = true;
        else if (!strcmp(argv[i], "--no-crest")) no_crest = true;
        else if (!strcmp(argv[i], "--frames")) frames = true;   // (every frame's size checked, a line each tenth of a second)
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
    io.Fonts->TexDesiredFormat = ImTextureFormat_RGBA32;
    const bool game_fonts = load_fonts(romfs, bundled);
    printf("fonts: %s\n", game_fonts ? "the game's own, from its files" : "Libertinus");
#ifdef HOOKSHOT_LOGO
    if (!no_crest) {
        crest = IM_NEW(ImTextureData)();
        crest->Create(ImTextureFormat_RGBA32, (int)hookshot_logo_w, (int)hookshot_logo_h);
        memcpy(crest->GetPixels(), hookshot_logo_rgba, (size_t)hookshot_logo_w * hookshot_logo_h * 4);
        crest->UseColors = true;
        ImGui::RegisterUserTexture(crest);
        crest_aspect = (float)hookshot_logo_w / hookshot_logo_h;
    }
#endif

    // (a first start as things are, the driver's part of the shaders off: two steps, their proportions as measured on an
    // RTX 4070 SUPER (translation 37 s, the predicted pipelines 6 to 44 s, the noted ones a fifth of a second) and shortened:
    // the shaders translated (2.2 to 26.2 s), the predicted pipelines (to 38.6 s), the noted ones (to 39.1 s))
    Story first;
    first.steps = {{0, 0, 14179, 2.2, 24}, {1, 0, 12513, 0.4, 12}, {2, 0, 3721, 0.1, 0.4}};
    Story three = first;   // (where the driver's part is switched on: three steps, it from 26.2 to 38.7 s)
    three.compile = true;
    three.steps = {{0, 0, 14179, 2.2, 24}, {0, 1, 31840, 0.5, 12}, {1, 0, 12513, 0.4, 9}, {2, 0, 3721, 0.1, 0.4}};
    Story warm;   // (any other start: the caches opened for three seconds, the noted pipelines read back in a moment)
    warm.steps = {{2, 0, 4028, 3.0, 0.25}};
    Story closed = first;   // (the window closed while the shaders are translated)
    closed.close_at = 10.3;
    Story nothing;   // (nothing to prepare, yet slow enough to show: the caches opened)
    nothing.steps = {{2, 0, 0, 1.8, 0}};
    const std::vector<Shot> shots = {
        {"01-opening", &first, 0.50},
        {"01b-opening-gleam", &first, 1.75},
        {"02-reading", &first, 2.05},
        {"03-translating", &first, 10.0},
        {"04-first-stone-ignites", &first, 26.44},
        {"05-building-pipelines", &first, 30.0},
        {"06-second-stone-ignites", &first, 34.4},
        {"06b-last-stone-ignites", &first, 38.85},
        {"07-starting-the-game", &first, 39.4},
        {"08-fade", &first, 39.65},
        {"09-warm-start", &warm, 1.02},
        {"09b-warm-start-stones-lit", &warm, 2.4},
        {"10-warm-start-done", &warm, 3.55},
        {"11-closing", &closed, 11.1},
        {"12-nothing-to-prepare", &nothing, 1.5},
        {"13-with-the-drivers-part", &three, 34.0},
    };
    int failed = 0;
    for (const Shot& shot : shots) {
        if (!only.empty() && only != shot.name) continue;
        start_screen::motion = start_screen::Motion{};
        ProgressClock eta;
        int eta_key = -1;
        Picture pic;
        const double step = 1.0 / 60;
        size_t most_vertices = 0;
        for (double t = 0;; t += step) {
            const bool last = t + step > shot.at;
            const Moment m = moment_of(*shot.story, last ? shot.at : t, eta, eta_key);
            io.DisplaySize = ImVec2((float)W, (float)H);
            io.DeltaTime = (float)step;
            ImGui::NewFrame();
            ui::Ctx c{ImGui::GetForegroundDrawList(), fonts, 1, (float)W, (float)H, 0, false};
            c.s = ui::unit((float)H, large ? 3 : 2, false, false, &c.safe);
            if (before) {
                OldView v = old_view(m.st);
                v.version = "v2.0.11 dev";
                draw_old(c, v, m.fade);
            } else {
                StartupView v = startup_view(m.st);
                v.version = "v2.0.11 dev", v.device = "NVIDIA GeForce RTX 4070 SUPER", v.linear = linear;
                start_screen::draw(c, v, m.fade, 100.0 + (last ? shot.at : t));
            }
            ImGui::Render();
            textures();
            const ImDrawData* dd = ImGui::GetDrawData();
            size_t vertices = 0;
            for (const ImDrawList* list : dd->CmdLists) {
                vertices += (size_t)list->VtxBuffer.Size;
                // (every corner inside the window, a little over for the soft edges: nothing is laid out off it)
                for (const ImDrawVert& vtx : list->VtxBuffer)
                    if (!(vtx.pos.x > -W && vtx.pos.x < 2 * W && vtx.pos.y > -H && vtx.pos.y < 2 * H)) failed++;
            }
            most_vertices = std::max(most_vertices, vertices);
            if (last) {
                pic.w = W, pic.h = H;
                pic.px.assign((size_t)W * H * 4, 255);
                for (size_t i = 0; i < pic.px.size(); i += 4) pic.px[i] = pic.px[i + 1] = pic.px[i + 2] = 0;
                rasterize(pic, dd);
                break;
            }
        }
        if (!write(pic, out + "/" + shot.name + ".rgba")) {
            fprintf(stderr, "cannot write into %s\n", out.c_str());
            return 1;
        }
        printf("%s: %dx%d at %.2f s, at most %zu vertices a frame\n", shot.name, W, H, shot.at, most_vertices);
        if (frames) printf("  (frames checked every 1/60 s up to it)\n");
    }
    if (!before) {   // (the end: the textures let go inside a frame that draws none of them, as the game does)
        start_screen::art_release();
        ImGui::NewFrame();
        ui::Ctx c{ImGui::GetForegroundDrawList(), fonts, 1, (float)W, (float)H, 0, false};
        c.s = ui::unit((float)H, 2, false, false, &c.safe);
        start_screen::draw(c, StartupView{}, 1, 1000);
        ImGui::Render();
        for (const ImDrawList* list : ImGui::GetDrawData()->CmdLists)
            for (const ImDrawCmd& cmd : list->CmdBuffer)
                if (cmd.ElemCount && cmd.TexRef._TexData && cmd.TexRef._TexData != io.Fonts->TexData) failed++, fprintf(stderr, "a released texture is drawn\n");
        textures();
        // (and they stay gone: ImGui asks again for a texture whose destroy it does not know to be meant, at the next frame)
        for (int k = 0; k < 3; k++) {
            ImGui::NewFrame();
            ImGui::Render();
            textures();
        }
        int gone = 0;
        for (ImTextureData* t : ImGui::GetPlatformIO().Textures) {
            if (t == crest || t == io.Fonts->TexData) continue;
            if (t->Status != ImTextureStatus_Destroyed || t->Pixels) failed++, fprintf(stderr, "a released texture is back (status %d)\n", (int)t->Status);
            else gone++;
        }
        if (gone != (crest ? 6 : 1)) failed++, fprintf(stderr, "%d textures released, %d expected\n", gone, crest ? 6 : 1);
    }
    if (failed) {
        fprintf(stderr, "FAIL: %d\n", failed);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
