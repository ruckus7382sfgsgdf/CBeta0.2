// (hookshot) Dear ImGui over the window's image (vk_imgui.cpp, third_party/imgui): the context, its Vulkan backend on the
// renderer's device, the fonts, and the textures. The Insert menu, the FPS counter and the start-up screen draw with it
// (vk_menu.cpp, vk_ui.h). Nothing here runs while nothing is shown: no ImGui frame, no render pass.
#pragma once

#include "vk_internal.h"

struct ImFont;
struct ImTextureData;

namespace mkwvk::ui {

struct Fonts {
    ImFont* body = nullptr;      // (the start-up screen's texts: Clarimo UD Regular of the game's files, else Libertinus)
    ImFont* strong = nullptr;    // (Clarimo UD DemiBold)
    ImFont* heading = nullptr;   // (titles: Cetra Display Medium, the game's "Special" font)
    ImFont* menu = nullptr;      // (Triforce: the game's menus' text, its "Normal" font, Cetra Display Regular; null: body.
                                 // The menu draws with menu as its body and heading as its strong: vk_menu.cpp menu_ctx)
    ImFont* glyph = nullptr;     // (Triforce: the game's button icons, its "External" font, U+E040 on; null: none)
    ImFont* number = nullptr;    // (Triforce: the game's HUD digits, NinDev-Number; null: none)
};
/// (Triforce) A texture of the game's own menu skin, read from the player's game files (vk_game_tex.h), w x h texels; null
/// until they are read (a thread of its own, from the first begin on) or when the files have none
struct Art {
    ImTextureData* tex = nullptr;
    float w = 0, h = 0;
};
const Art* art(const char* name);
/// (Triforce) The button icons' font asked for: read at the next begin, the one the game was served this run
/// (host/prompts.c: the controller's face) or the game's own
void want_glyphs();

/// A new ImGui frame for a w x h window image (the context made and the fonts loaded on the first call); false when ImGui
/// cannot draw here (no views of the window's images: the surface does not allow drawing into them)
bool begin(uint32_t w, uint32_t h);
const Fonts& fonts();
/// The start-up screen's logo (assets/hookshot.png), or null; its width over its height in *aspect
ImTextureData* logo(float* aspect);
/// The frame's drawing over the window's image `index` (TRANSFER_DST_OPTIMAL before and after): new glyphs and textures
/// copied in on cmd first (no wait on the queue), then one render pass
void record(VkCommandBuffer cmd, uint32_t index, uint32_t w, uint32_t h);
/// The same over another image (TRANSFER_DST_OPTIMAL before and after, colour-attachment use, a view of it; headless, the
/// shots' RGBA: vk_replay.cpp RX_VK_SHOT_MENU)
void record_to(VkCommandBuffer cmd, VkImage image, VkImageView view, uint32_t w, uint32_t h);
/// A present with nothing drawn: what record let go is destroyed once the GPU is done with it, all the same
void idle();
/// The glyphs (printable ASCII) of every font at these pixel sizes, baked a few sizes a frame from the next begin on: the
/// menu's sizes, before it is first opened (no hitch then). Replaces the sizes asked for before; these stay marked used on
/// every frame drawn, so ImGui does not let them go before the menu opens
void prebake(const float* px, int n);

} // namespace mkwvk::ui
