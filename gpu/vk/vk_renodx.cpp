// RenoDX is Carlos Lopez's (github.com/clshortfuse/renodx): Copyright (c) 2025 Carlos Lopez Jr.; its Zelda OoT mod,
// Copyright (C) 2026 Carlos Lopez. SPDX-License-Identifier: MIT. What follows is worked from that mod's addon: the
// notice and the license's text are in third_party/renodx/LICENSE.md.
//
// (local, RenoDX port) RenoDX's Zelda OoT mod (src/games/zelda-oot-oz, MIT, Carlos Lopez) inside the renderer: its replacement
// shaders (renodx_spv.h, from scripts/renodx_shaders.py) take the place of the game's scene tone mapping, HDR output encoder and
// fog when their modules are made (vk_shader.cpp make_module), and read RenoDX's settings from a second descriptor set, as its
// ReShade addon gives them (addon.cpp): set 1, bindings 0 and 1 the eye adaptation's state images (the one read, the one
// written), 2 its constants, 3 the anti-aliasing controls, 4 the settings (shared.h ShaderInjectData).
//
// The eye adaptation (adaptation.slangi) keeps its state in two 3 x 1 images, read from one and written to the other, swapped each
// present. One scene draw a frame is told to meter the picture and one fog draw to record the camera's field of view; each record
// carries its frame's generation, and a reader takes only the frame before's. The constants a frame's draws read are that frame's
// own (a ring of kRing frames of them, with a descriptor set each), since the GPU runs a frame's draws after the CPU has gone on.
//
// Not ported: the anti-aliasing and reconstruction shaders (Hookshot's own upscalers stand where they would go), the local
// exposure and SDR scene permutations, and the RGBA16F scene target the addon asks the game for.
// RX_VK_RENODX=0: off, the game's own shaders. RX_VK_RENODX_TRACE=1: every 1200th frame's draws by fragment module.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include "vk_menu.h"
#include "vk_renodx.h"

// RenoDX's shaders are not part of Hookshot: a build that asks for them fetches and compiles them (scripts/renodx_shaders.py
// into HOOKSHOT_RENODX's renodx_spv.h, which compiles this file with HOOKSHOT_RENODX). Without it only the stubs at the end
// are here: nothing is replaced, and the game keeps its own tone mapping and scene format.
#ifdef HOOKSHOT_RENODX

#include "renodx_spv.h"
#include "vk_work.h"   // (last: counts the commands that do GPU work, for full_barrier)

namespace mkwvk {
namespace {

// (shared.h ShaderInjectData: 17 floats, scalar layout)
struct Injection {
    float tone_map_type, peak_white_nits, diffuse_white_nits, graphics_white_nits;
    float tone_map_highlights, tone_map_shadows, tone_map_contrast, tone_map_saturation, tone_map_cone_contrast;
    float bloom_strength, custom_hue_clip, custom_flags, local_exposure, tone_map_exposure;
    float adaptation_target, adaptation_min_stops, adaptation_max_stops;
};
static_assert(sizeof(Injection) == 17 * sizeof(float));

constexpr size_t kShaders = sizeof RENODX_SHADERS / sizeof RENODX_SHADERS[0];
// (a draw's part in the eye adaptation, which picks its constants: renodx_spv.h's kinds 1 the fog, 2 the scene)
enum { kPlain = 0, kFog = 1, kMeter = 2, kKinds = 3 };
constexpr uint32_t kRing = 8;   // (frames of constants kept apart; even, so a frame's images follow from its place in the ring)
constexpr uint64_t kSlot = 256;   // (a uniform range's place: a multiple of any minUniformBufferOffsetAlignment)
constexpr uint64_t kInjectionAt = 0, kAaAt = kSlot, kSeedAt = 2 * kSlot, kAdaptationAt = 4 * kSlot;
constexpr uint64_t kBufferSize = kAdaptationAt + kRing * kKinds * kSlot;
constexpr uint32_t kGenerations = 1048576u;   // (shared.h RENODX_ADAPTATION_GENERATION_COUNT)

struct State {
    bool tried = false, ok = false;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet sets[kRing][kKinds]{};
    VkImage images[2]{};
    VmaAllocation image_allocs[2]{};
    VkImageView views[2]{};
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation buffer_alloc = nullptr;
    void* mapped = nullptr;
    std::chrono::steady_clock::time_point last_present = std::chrono::steady_clock::now();
};
State st;
std::mutex mu;
std::atomic<uint64_t> frame{0};   // (the frame being recorded: presents so far)
std::atomic<bool> meter_claimed{false}, fog_claimed{false};   // (this frame's one metering draw and one fog record)
std::atomic<int64_t> binds[kShaders];   // (draws through each of RenoDX's shaders since the last report)
std::atomic<int64_t> meters{0}, fogs{0};   // (frames metered and field-of-view records made since the last report)

bool enabled() {   // (Triforce, 11-13: the RenoDX HDR row as it was at the start; RX_VK_RENODX=0 or 1 over it)
    return renodx_chosen();
}
std::atomic<int> replaced_modules{0};   // (modules of the game's that RenoDX's took the place of, this run)
bool adapting() { return opts.rdx_tonemap && opts.rdx_midgray == 2; }

/// The settings from the Insert menu's rows, parsed as addon.cpp parses its own (0 to 100 with 50 neutral is 0 to 2), and the
/// constants of frame f's draws (delta: the time since the frame before)
void write_settings(uint64_t f, float delta_seconds) {
    Injection in{};
    in.tone_map_type = opts.rdx_tonemap ? 9.0f : 0.0f;   // (RENODX_TONE_MAP_TYPE_PSYCHOV35, _VANILLA)
    in.peak_white_nits = (float)opts.hdr_peak;
    in.diffuse_white_nits = (float)opts.hdr_white;
    in.graphics_white_nits = (float)opts.rdx_ui_white;
    in.tone_map_highlights = opts.rdx_highlights * 0.02f, in.tone_map_shadows = opts.rdx_shadows * 0.02f;
    in.tone_map_contrast = opts.rdx_contrast * 0.02f, in.tone_map_saturation = opts.rdx_saturation * 0.02f;
    in.tone_map_cone_contrast = 1.0f;
    in.bloom_strength = opts.rdx_bloom * 0.02f;
    in.custom_hue_clip = 0.5f;
    // (midgray: 0 the render's, 1 vanilla's, 16 the eye adaptation's; 2: match the tone slope; the colour space in bits 2 and 3)
    const uint32_t midgray = opts.rdx_midgray == 2 ? 16u : opts.rdx_midgray == 1 ? 1u : 0u;
    const uint32_t flags = midgray | 2u | ((uint32_t)std::clamp(opts.rdx_space, 0, 2) << 2);
    std::memcpy(&in.custom_flags, &flags, sizeof flags);
    in.local_exposure = 0.0f;
    in.tone_map_exposure = opts.rdx_exposure * 0.01f;
    in.adaptation_target = 0.18f, in.adaptation_min_stops = -4.0f, in.adaptation_max_stops = 2.0f;
    const float aa[2] = {1.0f, 0.0f};   // (AA strength, reconstruction: native)
    uint8_t* m = (uint8_t*)st.mapped;
    std::memcpy(m + kInjectionAt, &in, sizeof in);
    std::memcpy(m + kAaAt, aa, sizeof aa);
    for (int kind = 0; kind < kKinds; kind++) {   // (adaptation.slangi: delta_seconds, generation, capture_fov, meter_draw)
        const float adaptation[4] = {delta_seconds, (float)(f % kGenerations), kind == kFog ? 1.0f : 0.0f, kind == kMeter ? 1.0f : 0.0f};
        std::memcpy(m + kAdaptationAt + ((f % kRing) * kKinds + kind) * kSlot, adaptation, sizeof adaptation);
    }
    flush_mapped(st.buffer_alloc, kBufferSize);
}

/// The state images' seed copied in, on the replay's queue, waited for (once)
bool seed_images() {
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pi.queueFamilyIndex = dev.queue_family;
    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(dev.device, &pi, nullptr, &pool) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool, ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    bool ok = vkAllocateCommandBuffers(dev.device, &ai, &cmd) == VK_SUCCESS &&
              vkCreateFence(dev.device, &fi, nullptr, &fence) == VK_SUCCESS;
    if (ok) {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        for (const VkImage image : st.images) {
            VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            di.imageMemoryBarrierCount = 1, di.pImageMemoryBarriers = &b;
            vkCmdPipelineBarrier2(cmd, &di);
            const VkBufferImageCopy c{kSeedAt, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {3, 1, 1}};
            vkCmdCopyBufferToImage(cmd, st.buffer, image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        }
        vkEndCommandBuffer(cmd);
        VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        cb.commandBuffer = cmd;
        VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        si.commandBufferInfoCount = 1, si.pCommandBufferInfos = &cb;
        {
            std::lock_guard qk(dev.present_mu);   // (the queue's lock)
            queue_flush_locked();
            ok = vkQueueSubmit2(dev.queue, 1, &si, fence) == VK_SUCCESS;
        }
        if (ok) vkWaitForFences(dev.device, 1, &fence, VK_TRUE, UINT64_MAX);
    }
    if (fence) vkDestroyFence(dev.device, fence, nullptr);
    vkDestroyCommandPool(dev.device, pool, nullptr);
    return ok;
}

bool make() {
    const VkShaderStageFlags stages = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
    const VkDescriptorSetLayoutBinding bindings[5] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, stages, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, stages, nullptr},
        {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, stages, nullptr},
        {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, stages, nullptr},
        {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, stages, nullptr},
    };
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 5, li.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(dev.device, &li, nullptr, &st.layout) != VK_SUCCESS) return false;
    constexpr uint32_t kSets = kRing * kKinds;
    const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * kSets}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3 * kSets}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = kSets, pi.poolSizeCount = 2, pi.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(dev.device, &pi, nullptr, &st.pool) != VK_SUCCESS) return false;
    if (!create_buffer(kBufferSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, st.buffer,
                       st.buffer_alloc, &st.mapped) || !st.mapped)
        return false;
    // (addon.cpp's seed. Pixel 0: the linear anchor, log2 fast, log2 slow, generation; 1: the FOV capture; 2: the log2 filtered
    // target, the bleached fraction, reserved, generation. log2(0.18); the negative generations make these records stale)
    const float seed[12] = {0.18f, -2.473931188332412f, -2.473931188332412f, -1.0f, 0.0f, 0.0f, 0.0f, -2.0f,
                            -2.473931188332412f, 0.0f, 0.0f, -2.0f};
    std::memcpy((uint8_t*)st.mapped + kSeedAt, seed, sizeof seed);
    write_settings(frame.load(), 1.0f / 60.0f);
    for (int i = 0; i < 2; i++) {
        VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ic.imageType = VK_IMAGE_TYPE_2D, ic.format = VK_FORMAT_R32G32B32A32_SFLOAT, ic.extent = {3, 1, 1};
        ic.mipLevels = ic.arrayLayers = 1, ic.samples = VK_SAMPLE_COUNT_1_BIT, ic.tiling = VK_IMAGE_TILING_OPTIMAL;
        ic.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (!create_image(ic, st.images[i], st.image_allocs[i])) return false;
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = st.images[i], vi.viewType = VK_IMAGE_VIEW_TYPE_2D, vi.format = ic.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(dev.device, &vi, nullptr, &st.views[i]) != VK_SUCCESS) return false;
    }
    if (!seed_images()) return false;
    for (uint32_t ring = 0; ring < kRing; ring++) {
        for (int kind = 0; kind < kKinds; kind++) {
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = st.pool, ai.descriptorSetCount = 1, ai.pSetLayouts = &st.layout;
            if (vkAllocateDescriptorSets(dev.device, &ai, &st.sets[ring][kind]) != VK_SUCCESS) return false;
            // (the image the frame before wrote, then the one this frame writes)
            const VkDescriptorImageInfo images[2] = {{VK_NULL_HANDLE, st.views[ring & 1], VK_IMAGE_LAYOUT_GENERAL},
                                                     {VK_NULL_HANDLE, st.views[(ring & 1) ^ 1], VK_IMAGE_LAYOUT_GENERAL}};
            const VkDescriptorBufferInfo buffers[3] = {{st.buffer, kAdaptationAt + (ring * kKinds + kind) * kSlot, 16},
                                                       {st.buffer, kAaAt, 8},
                                                       {st.buffer, kInjectionAt, sizeof(Injection)}};
            VkWriteDescriptorSet w[5];
            for (uint32_t i = 0; i < 5; i++) {
                w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                w[i].dstSet = st.sets[ring][kind], w[i].dstBinding = i, w[i].descriptorCount = 1;
                if (i < 2) w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, w[i].pImageInfo = &images[i];
                else w[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, w[i].pBufferInfo = &buffers[i - 2];
            }
            vkUpdateDescriptorSets(dev.device, 5, w, 0, nullptr);
        }
    }
    return true;
}

bool ready() {
    std::lock_guard lk(mu);
    if (!st.tried) {
        st.tried = true;
        st.ok = make();
        logf(st.ok ? "vk: RenoDX: its settings and state made (set 1); peak %d nits, paper white %d nits"
                   : "vk: RenoDX: its settings could not be made: the replaced shaders read nothing there",
             opts.hdr_peak, opts.hdr_white);
    }
    return st.ok;
}

std::mutex trace_mu;
std::vector<uint32_t> traced;

} // namespace

std::atomic<bool> renodx_tracing{false};

const std::vector<uint32_t>* renodx_replacement(uint32_t crc) {
    if (!enabled()) return nullptr;
    static const std::unordered_map<uint32_t, std::vector<uint32_t>> table = [] {
        std::unordered_map<uint32_t, std::vector<uint32_t>> out;
        for (const auto& s : RENODX_SHADERS) out.emplace(s.crc, std::vector<uint32_t>(s.words, s.words + s.count));
        return out;
    }();
    const auto it = table.find(crc);
    if (it != table.end()) replaced_modules.fetch_add(1, std::memory_order_relaxed);
    return it == table.end() ? nullptr : &it->second;
}

VkDescriptorSetLayout renodx_set_layout() {
    return ready() ? st.layout : VK_NULL_HANDLE;
}

void renodx_bind(VkCommandBuffer cmd, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t crc) {
    if (!ready()) return;
    int kind = kPlain;
    for (size_t i = 0; i < kShaders; i++) {
        if (RENODX_SHADERS[i].crc != crc) continue;
        binds[i].fetch_add(1, std::memory_order_relaxed);
        // (addon.cpp PrepareAdaptationDraw, OnDrawAdaptationFog: with the eye adaptation on, the frame's first scene draw
        // meters and its first fog draw records the field of view)
        if (!adapting()) break;
        if (RENODX_SHADERS[i].kind == 2 && !meter_claimed.exchange(true)) kind = kMeter, meters.fetch_add(1, std::memory_order_relaxed);
        if (RENODX_SHADERS[i].kind == 1 && !fog_claimed.exchange(true)) kind = kFog, fogs.fetch_add(1, std::memory_order_relaxed);
        break;
    }
    vkCmdBindDescriptorSets(cmd, bp, layout, 1, 1, &st.sets[frame.load(std::memory_order_relaxed) % kRing][kind], 0, nullptr);
}

void renodx_trace_draw(uint32_t frag_crc) {
    std::lock_guard lk(trace_mu);
    traced.push_back(frag_crc);
}

void renodx_present() {
    {   // (RX_VK_RENODX_TRACE=1: every 1200th frame's draws, in order, by fragment module)
        static const bool trace = getenv("RX_VK_RENODX_TRACE") != nullptr;
        static int64_t frames;
        if (trace) {
            if (renodx_tracing.exchange(false)) {
                std::lock_guard lk(trace_mu);
                // (the frame's last 120 draws, 40 a line: the tone mapping and what follows it close the frame, and a
                // longer line is cut by the log)
                const size_t from = traced.size() > 120 ? traced.size() - 120 : 0;
                logf("vk: RenoDX trace: a frame of %zu draws; by fragment module from draw %zu:", traced.size(), from);
                for (size_t at = from; at < traced.size(); at += 40) {
                    std::string line;
                    char b[16];
                    for (size_t k = at; k < std::min(at + 40, traced.size()); k++) snprintf(b, sizeof b, " %08X", traced[k]), line += b;
                    logf("vk: RenoDX trace:%s", line.c_str());
                }
                traced.clear();
            }
            if (++frames % 1200 == 0) renodx_tracing.store(true);
        }
    }
    {   // (Triforce, 11-13) RenoDX chosen and nothing of the game's replaced half a minute in: said, once. Its shaders are
        // named by the CRC-32 of the module this translator makes, so another translation of the same program (a later
        // change to the emitter, another card's fixes) silently leaves the game's own HDR running
        static std::atomic<uint64_t> presents{0};
        if (enabled() && presents.fetch_add(1, std::memory_order_relaxed) == 1800 && !replaced_modules.load(std::memory_order_relaxed))
            logf("vk: RenoDX: on, and none of its four shaders has replaced one of the game's in 1800 presents: the game's own "
                 "picture runs. Either HDR is not Native on an HDR10 display (its shaders are the native HDR ones), or this "
                 "build names them by other checksums (scripts/renodx_shaders.py SHADERS: match them again)");
    }
    std::lock_guard lk(mu);
    if (!st.ok) return;
    const auto now = std::chrono::steady_clock::now();
    const float delta = std::chrono::duration<float>(now - st.last_present).count();
    st.last_present = now;
    // (the next frame: its constants written before any of its draws is recorded; its place in the ring was last read kRing
    // frames ago)
    const uint64_t next = frame.load() + 1;
    write_settings(next, delta);
    frame.store(next);
    meter_claimed.store(false), fog_claimed.store(false);
    // (how much of the picture goes through it. A scene draw and the output encoder each frame is the whole of it; the scene at 0
    // means a permutation of the game's tone mapping that RenoDX has no replacement for)
    if (next % 600 == 0) {
        char line[256];
        int n = 0;
        for (size_t i = 0; i < kShaders && n < (int)sizeof line - 32; i++)
            n += snprintf(line + n, sizeof line - n, " %08X %.2f", RENODX_SHADERS[i].crc,
                          (double)binds[i].exchange(0, std::memory_order_relaxed) / 600.0);
        logf("vk: RenoDX: draws a frame through its shaders over the last 600 presents:%s; eye adaptation %s, %lld frames metered, "
             "%lld field-of-view records",
             line, adapting() ? "on" : "off", (long long)meters.exchange(0), (long long)fogs.exchange(0));
    }
}
bool renodx_built() { return true; }

} // namespace mkwvk

#else  // (no RenoDX shaders in this build: nothing is replaced)

namespace mkwvk {

std::atomic<bool> renodx_tracing{false};
bool renodx_built() { return false; }
const std::vector<uint32_t>* renodx_replacement(uint32_t) { return nullptr; }
VkDescriptorSetLayout renodx_set_layout() { return VK_NULL_HANDLE; }
void renodx_bind(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t) {}
void renodx_trace_draw(uint32_t) {}
void renodx_present() {}

} // namespace mkwvk

#endif
