// (Triforce) The game's own UI textures, read from the player's game files, in memory only (vk_imgui.cpp; never written
// anywhere, never in the repository or a binary). romfs/UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.bntx.zs is zstd with
// the dictionary zs.zsdic (vk_game_font.h Reader); in it a BNTX: a table of BRTI records, each a texture by name. The menus'
// textures are one level, 2D, in two formats: BC4 (0x1d01: one channel, a mask the layout's material colours) and ASTC 4x4
// (0x2d06 sRGB, 0x2d01), stored in the console's block-linear order (the Switch 2's: third_party/suyu decoders.cpp). No
// Vulkan or ImGui here: tests/ui_preview.cpp draws the menu with the same textures offline.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "video_core/textures/astc.h"
#include "video_core/textures/decoders.h"

namespace mkwvk::game_tex {

struct Image {
    uint32_t w = 0, h = 0;
    bool mask = false;   // (BC4: one channel, in every byte of rgba's pixels' alpha; its colour white)
    bool srgb = false;
    std::vector<unsigned char> rgba;
};

/// A BC4 block's 16 values (4 x 4, row by row)
inline void bc4_block(const unsigned char* b, unsigned char out[16]) {
    unsigned char v[8];
    v[0] = b[0], v[1] = b[1];
    if (v[0] > v[1]) {
        for (int i = 1; i < 7; i++) v[i + 1] = (unsigned char)(((7 - i) * v[0] + i * v[1] + 3) / 7);
    } else {
        for (int i = 1; i < 5; i++) v[i + 1] = (unsigned char)(((5 - i) * v[0] + i * v[1] + 2) / 5);
        v[6] = 0, v[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++) out[i] = v[bits >> (3 * i) & 7];
}

class Bntx {
public:
    struct Entry {
        std::string name;
        uint32_t w = 0, h = 0, format = 0, size = 0, block_height = 0;
        uint64_t data = 0;
    };
    /// The archive's table (the bytes are kept: decode reads them)
    bool parse(std::vector<unsigned char>&& bytes) {
        d = std::move(bytes);
        entries.clear();
        if (d.size() < 0x60 || memcmp(d.data(), "BNTX", 4)) return false;
        const uint32_t count = u32(0x24);
        const uint64_t table = u64(0x28);
        if (count > 65536 || table > d.size() || d.size() - table < (uint64_t)count * 8) return false;
        for (uint32_t i = 0; i < count; i++) {
            const uint64_t p = u64(table + 8ull * i);
            if (p > d.size() || d.size() - p < 0xA0 || memcmp(d.data() + p, "BRTI", 4)) return false;
            Entry e;
            e.format = u32(p + 0x1C), e.w = u32(p + 0x24), e.h = u32(p + 0x28);
            e.block_height = u32(p + 0x34) & 7;
            e.size = u32(p + 0x50);
            const uint64_t name = u64(p + 0x60), mips = u64(p + 0x70);
            if (name + 2 > d.size() || mips + 8 > d.size()) return false;
            const size_t len = d[name] | d[name + 1] << 8;
            if (name + 2 + len > d.size()) return false;
            e.name.assign((const char*)d.data() + name + 2, len);
            e.data = u64(mips);
            const bool one_level_2d = u32(p + 0x2C) <= 1 && u32(p + 0x30) <= 1;
            if (!one_level_2d || e.data > d.size() || d.size() - e.data < e.size) e.size = 0;   // (not one this reads)
            entries.push_back(std::move(e));
        }
        return true;
    }
    const Entry* find(const char* name) const {
        for (const Entry& e : entries)
            if (e.name == name) return &e;
        return nullptr;
    }
    /// A texture's pixels (RGBA, 8 bits each): false for a format or a shape this does not read
    bool decode(const Entry& e, Image& out) const {
        const uint32_t kind = e.format >> 8, bw = (e.w + 3) / 4, bh = (e.h + 3) / 4;
        const uint32_t bytes = kind == 0x1d ? 8 : kind == 0x2d ? 16 : 0;
        if (!bytes || !e.size || !e.w || !e.h || e.w > 8192 || e.h > 8192) return false;
        std::vector<unsigned char> blocks((size_t)bw * bh * bytes);
        // (the console's block-linear order to rows of blocks)
        if (Tegra::Texture::CalculateSize(true, bytes, bw, bh, 1, e.block_height, 0) > e.size) return false;
        Tegra::Texture::UnswizzleTexture(std::span<unsigned char>(blocks), std::span<const unsigned char>(d.data() + e.data, e.size), bytes,
                                         bw, bh, 1, e.block_height, 0, 0);
        out.w = e.w, out.h = e.h, out.mask = kind == 0x1d, out.srgb = (e.format & 0xff) == 6;
        out.rgba.assign((size_t)e.w * e.h * 4, 255);
        if (kind == 0x1d) {
            for (uint32_t by = 0; by < bh; by++)
                for (uint32_t bx = 0; bx < bw; bx++) {
                    unsigned char v[16];
                    bc4_block(blocks.data() + ((size_t)by * bw + bx) * 8, v);
                    for (uint32_t y = 0; y < 4 && by * 4 + y < e.h; y++)
                        for (uint32_t x = 0; x < 4 && bx * 4 + x < e.w; x++)
                            out.rgba[((size_t)(by * 4 + y) * e.w + bx * 4 + x) * 4 + 3] = v[y * 4 + x];
                }
            return true;
        }
        Tegra::Texture::ASTC::DecompressSerial(std::span<const unsigned char>(blocks), e.w, e.h, 1, 4, 4, std::span<unsigned char>(out.rgba));
        return true;
    }
    std::vector<Entry> entries;

private:
    uint32_t u32(uint64_t o) const {
        uint32_t v;
        memcpy(&v, d.data() + o, 4);
        return v;
    }
    uint64_t u64(uint64_t o) const {
        uint64_t v;
        memcpy(&v, d.data() + o, 8);
        return v;
    }
    std::vector<unsigned char> d;
};

// ---- the menus' skin: the textures the Options screen is drawn with (vk_ui.h), by the game's own names, and a few made
// from them here as the game's layout shader makes them at each draw: a plate is its shape (BtnThinBase_00's alpha), its
// colour between two browns by the grain texture (BtnThinBase_05), its fine frame line (BtnThinBase_04, or _02 with the
// curls at its corners) over it. The browns are measured from the game's own screens (the Load screen's buttons).
struct Skin {
    struct Entry {
        std::string name;
        Image img;
    };
    std::vector<Entry> images;
    const Image* get(const char* name) const {
        for (const Entry& e : images)
            if (e.name == name) return &e.img;
        return nullptr;
    }
};
struct Rgb {
    unsigned char r, g, b;
};
// (the plates' browns: the grain's dark and light ends, and the frame line's; the selected row's a little lighter)
// (measured on the game's Options rows: 98,74,37 at a plate's rim to 111,84,44 at its middle, its edge line 122,103,61; the
// selected row's plate is the same: the game marks the selection with the cursor alone)
constexpr Rgb kPlateDark{96, 72, 36}, kPlateLight{112, 85, 45}, kPlateLine{124, 104, 62};
constexpr Rgb kPlateSelDark = kPlateDark, kPlateSelLight = kPlateLight, kPlateSelLine = kPlateLine;

/// A plate from the game's three layers (shape, grain, line): the same size as the shape
inline Image make_plate(const Image& shape, const Image& grain, const Image* line, Rgb dark, Rgb light, Rgb line_col) {
    Image out;
    out.w = shape.w, out.h = shape.h, out.srgb = true;
    out.rgba.resize((size_t)out.w * out.h * 4);
    for (uint32_t y = 0; y < out.h; y++)
        for (uint32_t x = 0; x < out.w; x++) {
            const size_t i = ((size_t)y * out.w + x) * 4;
            const uint32_t gx = (std::min)(x, grain.w - 1), gy = (std::min)(y, grain.h - 1);
            const float g = grain.rgba[((size_t)gy * grain.w + gx) * 4] / 255.0f;
            float c[3] = {dark.r + (light.r - dark.r) * g, dark.g + (light.g - dark.g) * g, dark.b + (light.b - dark.b) * g};
            if (line && x < line->w && y < line->h) {
                const float a = line->rgba[((size_t)y * line->w + x) * 4 + 3] / 255.0f;
                c[0] += (line_col.r - c[0]) * a, c[1] += (line_col.g - c[1]) * a, c[2] += (line_col.b - c[2]) * a;
            }
            out.rgba[i] = (unsigned char)(c[0] + 0.5f), out.rgba[i + 1] = (unsigned char)(c[1] + 0.5f), out.rgba[i + 2] = (unsigned char)(c[2] + 0.5f);
            out.rgba[i + 3] = shape.rgba[i + 3];
        }
    return out;
}

/// The cursor from the game's two masks, as its layout shader draws it: OptionBtnThinCursorLine is a ramp from the frame's
/// outer edge inwards (0 to 1 over 28 texels: a distance, not a picture of a line), the line a band of it; the glow
/// (OptionBtnThinCursorGlow, a wider ramp) shows outside the line. The band is measured from the game's own cursor on its
/// Options screen (1920 x 1080: the line 13 layout units thick, its inner edge a box of 809 x 114 units in the 960 x 260
/// pane: the ramp from 0.34 to 0.65; about 20 units of blue-white glow outside it, nothing inside).
/// *line and *glow: white, their strength in alpha (the glow is tinted and breathes at the draw)
inline void make_cursor(const Image& ramp, const Image& wide, Image* line, Image* glow) {
    auto step = [](float v, float a, float b) {
        const float t = std::clamp((v - a) / (b - a), 0.0f, 1.0f);
        return t * t * (3 - 2 * t);
    };
    for (Image* o : {line, glow}) o->w = ramp.w, o->h = ramp.h, o->mask = true, o->rgba.assign((size_t)ramp.w * ramp.h * 4, 255);
    for (size_t i = 0; i < (size_t)ramp.w * ramp.h; i++) {
        const float l = ramp.rgba[i * 4 + 3] / 255.0f;
        const float g = i < (size_t)wide.w * wide.h ? wide.rgba[i * 4 + 3] / 255.0f : 0;
        const float la = step(l, 0.31f, 0.37f) * (1 - step(l, 0.62f, 0.69f));
        const float outside = std::pow(std::clamp(g / 0.36f, 0.0f, 1.0f), 1.6f) * (1 - step(l, 0.31f, 0.37f));
        line->rgba[i * 4 + 3] = (unsigned char)(la * 255 + 0.5f);
        glow->rgba[i * 4 + 3] = (unsigned char)(outside * 255 + 0.5f);
    }
}

/// The skin from the archive's bytes (a BNTX): false when it is not the game's (a texture of the list missing)
inline bool make_skin(std::vector<unsigned char>&& bntx_bytes, Skin& skin) {
    static const char* const names[] = {
        "SystemHeader_00", "SystemBg_05", "SystemCaptionBg_00", "SystemGuideBase_00", "BtnThinBase_00", "BtnThinBase_01",
        "BtnThinBase_02", "BtnThinBase_04", "BtnThinBase_05", "OptionBtnThinElement_00", "OptionBtnThinCursorLine_00",
        "OptionBtnThinCursorGlow_00", "ScrollBarBase_00", "ScrollBar_00", "OptionSeparateMark_00",
        "SystemLine_03", "MenuHeaderSelect_00", "OptionCheckMark_00"};
    Bntx bntx;
    if (!bntx.parse(std::move(bntx_bytes))) return false;
    skin.images.clear();
    for (const char* n : names) {
        const Bntx::Entry* e = bntx.find(n);
        Skin::Entry out;
        out.name = n;
        if (!e || !bntx.decode(*e, out.img)) return false;
        skin.images.push_back(std::move(out));
    }
    const Image *shape = skin.get("BtnThinBase_00"), *grain = skin.get("BtnThinBase_05");
    Skin::Entry row{"plate_row", make_plate(*shape, *grain, skin.get("BtnThinBase_04"), kPlateDark, kPlateLight, kPlateLine)};
    Skin::Entry row_sel{"plate_row_sel", make_plate(*shape, *grain, skin.get("BtnThinBase_04"), kPlateSelDark, kPlateSelLight, kPlateSelLine)};
    Skin::Entry btn{"plate_btn", make_plate(*shape, *grain, skin.get("BtnThinBase_02"), kPlateDark, kPlateLight, kPlateLine)};
    skin.images.push_back(std::move(row)), skin.images.push_back(std::move(row_sel)), skin.images.push_back(std::move(btn));
    Skin::Entry line{"cursor_line", {}}, glow{"cursor_glow", {}};
    make_cursor(*skin.get("OptionBtnThinCursorLine_00"), *skin.get("OptionBtnThinCursorGlow_00"), &line.img, &glow.img);
    skin.images.push_back(std::move(line)), skin.images.push_back(std::move(glow));
    return true;
}

} // namespace mkwvk::game_tex
