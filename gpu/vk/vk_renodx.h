// (local, RenoDX port) RenoDX's Zelda OoT shaders in place of the game's own tone mapping and HDR output (vk_renodx.cpp).
#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "vk_internal.h"

namespace mkwvk {

/// Whether this build has RenoDX's shaders in it (HOOKSHOT_RENODX, scripts/renodx_shaders.py): the Insert menu's RenoDX rows
bool renodx_built();
/// RenoDX's module for the one of Hookshot's whose CRC-32 this is, or null (none, or RX_VK_RENODX=0)
const std::vector<uint32_t>* renodx_replacement(uint32_t crc);
/// Set 1 of a program with a replaced stage (RenoDX's settings, its eye adaptation's state); null: it could not be made
VkDescriptorSetLayout renodx_set_layout();
/// Before a draw of such a program (crc: its replaced module's, Stage::renodx): set 1 bound
void renodx_bind(VkCommandBuffer cmd, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t crc);
/// (RX_VK_RENODX_TRACE=1) while set, one frame's draws are noted by their fragment module's CRC-32, and logged at its present:
/// which of the game's tone-mapping permutations a scene uses
extern std::atomic<bool> renodx_tracing;
void renodx_trace_draw(uint32_t frag_crc);
/// Each present: the settings as the menu has them now
void renodx_present();

} // namespace mkwvk
