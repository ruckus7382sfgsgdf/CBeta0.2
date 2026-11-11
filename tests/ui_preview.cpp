// (Triforce) The menu, the FPS counter and the notice as vk_menu.cpp draws them, into picture files, without the game and
// without a GPU: gpu/vk/vk_menu.cpp itself, compiled as it is, with a stand-in for vk_imgui.cpp that rasterizes Dear ImGui's
// draw lists in software (textured triangles, source-over, as the window's UNORM image blends them). The fonts and the
// textures are the game's own, read from a romfs folder at run time as the game's build reads them (vk_game_font.h,
// vk_game_tex.h): none is in this file or in what it writes besides the pictures themselves.
//   ui_preview <romfs folder> <width> <height> <background.rgba or -> <out.rgba> <menu keys> [fps <shown> <game>]
// background: width x height RGBA bytes (a screenshot of the game), or - for black. menu keys: RX_VK_MENU_SCRIPT's names with
// commas ("INS,TABR,DOWN"), each a frame's tick; "-" none. The options are triforce_options.ini beside this program.
// Built by tests/ui_preview_build.ps1 (the lab's tools folder has the script that makes the comparison pictures).
#define NOMINMAX
#include <windows.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"
#include "vk_game_font.h"
#include "vk_game_tex.h"
#include "vk_imgui.h"
#include "vk_menu.h"
#include "vk_dlss.h"
#include "vk_fsr4.h"

// ---- what vk_menu.cpp takes from the rest of the back end: stand-ins ----
namespace mkwvk {
Device dev;
std::vector<std::string> gpu_device_names = {"NVIDIA GeForce RTX 4070 SUPER", "Intel(R) UHD Graphics 770"};
static std::string romfs_path;
static const char* romfs_dir_fn() { return romfs_path.c_str(); }
static MkwVkHost host_stub{};
// (the controls, for the Controls page: the Switch's own layout on a pad, a usual set of keys)
static int binding_fn(int set, int button, int slot) {
    static const int pad[HOS_SW_N] = {HOS_PAD_EAST, HOS_PAD_SOUTH, HOS_PAD_NORTH, HOS_PAD_WEST, HOS_PAD_LSTICK, HOS_PAD_RSTICK, HOS_PAD_LSHOULDER,
                                      HOS_PAD_RSHOULDER, HOS_PAD_LTRIGGER, HOS_PAD_RTRIGGER, HOS_PAD_START, HOS_PAD_BACK, HOS_PAD_LEFT, HOS_PAD_UP,
                                      HOS_PAD_RIGHT, HOS_PAD_DOWN};
    static const int keys[HOS_KEY_N] = {'E', VK_SPACE, 'Q', 'F', 'C', 'V', '1', '2', VK_SHIFT, VK_LBUTTON, VK_RETURN, VK_TAB, VK_LEFT, VK_UP,
                                        VK_RIGHT, VK_DOWN, 'W', 'S', 'A', 'D', 'I', 'K', 'J', 'L'};
    if (set == HOS_BIND_PAD) return button >= 0 && button < HOS_SW_N ? pad[button] : -1;
    return slot == 0 && button >= 0 && button < HOS_KEY_N ? keys[button] : 0;
}
static void bind_fn(int, int, int, int) {}
static uint32_t pad_raw_fn() { return 0; }
const MkwVkHost* host = &host_stub;
// (a window that is not one: the restart question is asked only with a window to close; nothing sent to it arrives)
HWND app_window() { return (HWND)(intptr_t)1; }
void logf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
bool antilag_available() { return false; }
bool reflex_available() { return true; }
bool dlss_built() { return false; }
void dlss_probe() {}
DlssWhy dlss_why() { return (DlssWhy)1; }
bool fsr4_built() { return false; }
void fsr4_probe() {}
FsrWhy fsr4_why() { return (FsrWhy)1; }
bool fsr3_wanted() { return false; }
int temporal_mode() { return 0; }
void pipeline_cache_save(bool) {}
namespace fsr3 {
bool supported() { return true; }
}
namespace fsr3fi {
unsigned frames_wanted() { return 0; }
}
int64_t prof_ns[PROF_COUNT];
} // namespace mkwvk

// ---- the stand-in for vk_imgui.cpp ----
namespace mkwvk::ui {
namespace {
Fonts fonts_;
std::vector<std::pair<std::string, Art>> arts_;
std::vector<unsigned char> target;   // (RGBA, the picture drawn over)
uint32_t tw = 0, th = 0;
bool made = false;

ImFont* add_font(game_font::Reader& r, const char* name, bool latin) {
    std::vector<unsigned char> bytes = r.read(name, latin);
    if (bytes.empty()) return nullptr;
    void* font = IM_ALLOC(bytes.size());
    memcpy(font, bytes.data(), bytes.size());
    ImFontConfig cfg;
    snprintf(cfg.Name, sizeof cfg.Name, "%s", name);
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(font, (int)bytes.size(), 18.0f, &cfg);
}
void init() {
    made = true;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.Fonts->TexDesiredFormat = ImTextureFormat_RGBA32;
    game_font::Reader reader;
    if (!reader.open(romfs_path)) {
        fprintf(stderr, "no game files at %s\n", romfs_path.c_str());
        exit(2);
    }
    fonts_.body = add_font(reader, "ClarimoUDPE-Regular", true);   // (as vk_imgui.cpp load_fonts)
    fonts_.strong = add_font(reader, "ClarimoUDPE-DemiBold", true);
    fonts_.heading = add_font(reader, "CetraDisplayPro-Medium", true);
    fonts_.menu = add_font(reader, "CetraDisplayPro-Regular", true);
    fonts_.number = add_font(reader, "NinDev-Number", false);
    if (!getenv("UI_PREVIEW_NO_GLYPHS")) fonts_.glyph = add_font(reader, "External", false);
    if (!fonts_.body || !fonts_.strong || !fonts_.heading || !fonts_.menu) {
        fprintf(stderr, "the game's fonts were not read\n");
        exit(2);
    }
    if (!getenv("UI_PREVIEW_NO_ART")) {
        std::vector<unsigned char> bytes;
        game_tex::Skin skin;
        if (!reader.read_packed("UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.bntx.zs", bytes) || !game_tex::make_skin(std::move(bytes), skin)) {
            fprintf(stderr, "the game's menu textures were not read\n");
            exit(2);
        }
        for (game_tex::Skin::Entry& e : skin.images) {
            ImTextureData* t = IM_NEW(ImTextureData)();
            t->Create(ImTextureFormat_RGBA32, (int)e.img.w, (int)e.img.h);
            memcpy(t->GetPixels(), e.img.rgba.data(), e.img.rgba.size());
            t->UseColors = true;
            ImGui::RegisterUserTexture(t);
            arts_.push_back({e.name, Art{t, (float)e.img.w, (float)e.img.h}});
        }
    }
}

// ---- the rasterizer ----
struct Sampler {
    const unsigned char* px = nullptr;
    int w = 0, h = 0, bpp = 4;
    void at(float u, float v, float out[4]) const {
        if (!px) {
            out[0] = out[1] = out[2] = out[3] = 1;
            return;
        }
        const float x = u * w - 0.5f, y = v * h - 0.5f;
        const int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
        const float fx = x - x0, fy = y - y0;
        float acc[4] = {0, 0, 0, 0};
        for (int j = 0; j < 2; j++)
            for (int i = 0; i < 2; i++) {
                const int xi = std::clamp(x0 + i, 0, w - 1), yi = std::clamp(y0 + j, 0, h - 1);
                const float k = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
                const unsigned char* p = px + ((size_t)yi * w + xi) * bpp;
                if (bpp == 4) for (int c = 0; c < 4; c++) acc[c] += p[c] * k;
                else acc[0] += 255 * k, acc[1] += 255 * k, acc[2] += 255 * k, acc[3] += p[0] * k;
            }
        for (int c = 0; c < 4; c++) out[c] = acc[c] / 255.0f;
    }
};
void triangle(const ImDrawVert& a, const ImDrawVert& b, const ImDrawVert& c, const Sampler& tex, const ImVec4& clip) {
    const float area = (b.pos.x - a.pos.x) * (c.pos.y - a.pos.y) - (b.pos.y - a.pos.y) * (c.pos.x - a.pos.x);
    if (area == 0) return;
    const int x0 = std::max((int)std::floor(std::min({a.pos.x, b.pos.x, c.pos.x})), (int)std::max(clip.x, 0.0f));
    const int x1 = std::min((int)std::ceil(std::max({a.pos.x, b.pos.x, c.pos.x})), (int)std::min(clip.z, (float)tw));
    const int y0 = std::max((int)std::floor(std::min({a.pos.y, b.pos.y, c.pos.y})), (int)std::max(clip.y, 0.0f));
    const int y1 = std::min((int)std::ceil(std::max({a.pos.y, b.pos.y, c.pos.y})), (int)std::min(clip.w, (float)th));
    const ImVec4 ca = ImGui::ColorConvertU32ToFloat4(a.col), cb = ImGui::ColorConvertU32ToFloat4(b.col), cc = ImGui::ColorConvertU32ToFloat4(c.col);
    const bool flat = a.col == b.col && b.col == c.col;
    const bool one_texel = a.uv.x == b.uv.x && a.uv.x == c.uv.x && a.uv.y == b.uv.y && a.uv.y == c.uv.y;
    float texel[4] = {1, 1, 1, 1};
    if (one_texel) tex.at(a.uv.x, a.uv.y, texel);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            const float px = x + 0.5f, py = y + 0.5f;
            float w0 = ((b.pos.x - px) * (c.pos.y - py) - (b.pos.y - py) * (c.pos.x - px)) / area;
            float w1 = ((c.pos.x - px) * (a.pos.y - py) - (c.pos.y - py) * (a.pos.x - px)) / area;
            float w2 = 1 - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            float t[4];
            if (one_texel) memcpy(t, texel, sizeof t);
            else tex.at(a.uv.x * w0 + b.uv.x * w1 + c.uv.x * w2, a.uv.y * w0 + b.uv.y * w1 + c.uv.y * w2, t);
            const float col[4] = {flat ? ca.x : ca.x * w0 + cb.x * w1 + cc.x * w2, flat ? ca.y : ca.y * w0 + cb.y * w1 + cc.y * w2,
                                  flat ? ca.z : ca.z * w0 + cb.z * w1 + cc.z * w2, flat ? ca.w : ca.w * w0 + cb.w * w1 + cc.w * w2};
            const float al = t[3] * col[3];
            if (al <= 0) continue;
            unsigned char* d = target.data() + ((size_t)y * tw + x) * 4;
            for (int k = 0; k < 3; k++) d[k] = (unsigned char)std::clamp(d[k] * (1 - al) + t[k] * col[k] * al * 255.0f + 0.5f, 0.0f, 255.0f);
            d[3] = 255;
        }
}
void raster(ImDrawData* dd) {
    if (dd->Textures)
        for (ImTextureData* tex : *dd->Textures)
            if (tex->Status != ImTextureStatus_OK && tex->Status != ImTextureStatus_Destroyed) {
                if (tex->Status == ImTextureStatus_WantDestroy) tex->SetTexID(ImTextureID_Invalid), tex->SetStatus(ImTextureStatus_Destroyed);
                else tex->SetTexID((ImTextureID)(intptr_t)1), tex->SetStatus(ImTextureStatus_OK);
            }
    for (ImDrawList* list : dd->CmdLists)
        for (const ImDrawCmd& cmd : list->CmdBuffer) {
            if (cmd.UserCallback) continue;
            Sampler s;
            if (ImTextureData* t = cmd.TexRef._TexData) s.px = (const unsigned char*)t->GetPixels(), s.w = t->Width, s.h = t->Height, s.bpp = t->BytesPerPixel;
            const ImDrawIdx* idx = list->IdxBuffer.Data + cmd.IdxOffset;
            const ImDrawVert* vtx = list->VtxBuffer.Data + cmd.VtxOffset;
            for (unsigned i = 0; i + 2 < cmd.ElemCount; i += 3) triangle(vtx[idx[i]], vtx[idx[i + 1]], vtx[idx[i + 2]], s, cmd.ClipRect);
        }
}
} // namespace

bool begin(uint32_t w, uint32_t h) {
    if (!made) init();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    return true;
}
const Fonts& fonts() { return fonts_; }
ImTextureData* logo(float* aspect) {
    *aspect = 1;
    return nullptr;
}
const Art* art(const char* name) {
    for (const auto& a : arts_)
        if (a.first == name) return &a.second;
    return nullptr;
}
void want_glyphs() {}
void prebake(const float*, int) {}
void idle() {}
void record_to(VkCommandBuffer, VkImage, VkImageView, uint32_t, uint32_t) {
    ImGui::Render();
    raster(ImGui::GetDrawData());
}
void record(VkCommandBuffer cmd, uint32_t, uint32_t w, uint32_t h) { record_to(cmd, VK_NULL_HANDLE, VK_NULL_HANDLE, w, h); }
} // namespace mkwvk::ui

int main(int argc, char** argv) {
    if (argc < 7) {
        fprintf(stderr, "ui_preview <romfs> <width> <height> <background.rgba or -> <out.rgba> <keys or -> [fps <shown> <game>]\n");
        return 1;
    }
    using namespace mkwvk;
    romfs_path = argv[1];
    host_stub.romfs_dir = romfs_dir_fn;
    host_stub.binding = binding_fn, host_stub.bind = bind_fn, host_stub.pad_raw = pad_raw_fn;
    ui::tw = (uint32_t)atoi(argv[2]), ui::th = (uint32_t)atoi(argv[3]);
    dev.swap_extent = {ui::tw, ui::th};
    ui::target.assign((size_t)ui::tw * ui::th * 4, 0);
    for (size_t i = 3; i < ui::target.size(); i += 4) ui::target[i] = 255;
    if (strcmp(argv[4], "-")) {
        FILE* f = fopen(argv[4], "rb");
        if (!f || fread(ui::target.data(), 1, ui::target.size(), f) != ui::target.size()) {
            fprintf(stderr, "background %s: not %ux%u RGBA\n", argv[4], ui::tw, ui::th);
            return 2;
        }
        fclose(f);
    }
    // (the keys as RX_VK_MENU_SCRIPT has them, all due at once: menu_tick takes one a call)
    std::string script;
    int ticks = 2;
    if (strcmp(argv[6], "-")) {
        for (const char* p = argv[6]; *p;) {
            const char* e = strchr(p, ',');
            const std::string key(p, e ? (size_t)(e - p) : strlen(p));
            script += (script.empty() ? "" : ",") + std::string("0:") + key;
            ticks++;
            p = e ? e + 1 : p + key.size();
        }
        SetEnvironmentVariableA("RX_VK_MENU_SCRIPT", script.c_str());
        _putenv_s("RX_VK_MENU_SCRIPT", script.c_str());
    }
    _putenv_s("RX_VK_UI_STILL", "1");
    _putenv_s("HOS_FULLSCREEN", "0");   // (the Fullscreen row left alone: there is no window to ask)
    menu_init();
    menu_set_gpu_count(2);
    if (argc >= 10 && !strcmp(argv[7], "fps")) {   // (the counter: the frames of a second counted, at these rates)
        opts.show_fps = 1;
        const double shown = atof(argv[8]), game = atof(argv[9]);
        if (game > 0 && game != shown) opts.framegen = 2;
        menu_tick();
        const int64_t t0 = prof_now();
        Sleep(1000);
        const double secs = (prof_now() - t0) / 1e9;
        menu_note_frame(false, (int)std::lround(shown * secs));
        for (int i = 0; i < (int)std::lround((game > 0 ? game : shown) * secs); i++) menu_note_frame(true, 0);
    }
    // (UI_PREVIEW_GAME_OPTIONS=1: as inside the game's own Options screen: the host's word each tick, as host/oot_menu.c gives it)
    const bool game_options = getenv("UI_PREVIEW_GAME_OPTIONS") != nullptr;
    if (game_options) menu_option_set(HOS_OPT_GAME_OPTIONS, 1), menu_tick();
    for (int i = 0; i < ticks; i++) {
        if (game_options) menu_option_set(HOS_OPT_GAME_OPTIONS, 1);
        menu_tick();
    }
    // (two frames: the first lays out and bakes the glyphs, the second is the picture, the scroll settled)
    std::vector<unsigned char> clean = ui::target;
    for (int frame = 0; frame < 3; frame++) {
        ui::target = clean;
        if (game_options) menu_option_set(HOS_OPT_GAME_OPTIONS, 1);
        menu_record_image(nullptr, (VkImage)1, (VkImageView)1, ui::tw, ui::th);
        if (!ui::made) {   // (nothing shows: the picture as it came)
            break;
        }
        Sleep(20);
    }
    FILE* f = fopen(argv[5], "wb");
    if (!f) return 3;
    fwrite(ui::target.data(), 1, ui::target.size(), f);
    fclose(f);
    return 0;
}
