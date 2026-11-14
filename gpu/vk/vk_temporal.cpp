// (hookshot) A temporal upscaler in place of OoT's gameplay upscale network: the frame chain (vk_temporal.h), shared by the
// engines that draw with it, NVIDIA DLSS (vk_dlss.cpp) and AMD FSR 4 (vk_fsr4.cpp).
// The chain is hckrman101's, from their NVIDIA DLSS (#841): the camera, the inputs pass, the actor motion and the jitter's
// state, moved here from vk_dlss.cpp unchanged in what they do, so that DLSS and FSR share them.
//
// OoT has no temporal upscaler of its own: its scene is drawn at a lower size and a network of its own (the program
// fd029a317a9a4b94 at 3/4, another for each other share; one dispatch, cooperative matrices) scales it up, from the
// square root of the scene's tonemapped colour, into the frame's B10G11R11 picture (before the HUD). Here that dispatch is replaced:
//  - the scene's camera is taken from the draws into the scene's colour target: their vertex stage's uniform buffer 3
//    holds the target's size at 0x120, this frame's view-projection at 0x30 (row major, clip = M * position; the projection's w row ends -1, 0 at 0xa8). The largest such target in a frame is
//    the scene (reflections and half-size passes have the same layout). The scene's depth is that draw's depth target.
//  - shaders/dlss_prep.comp makes the engine's inputs at the render size: the colour as the network samples it (LDR), the
//    depth, and motion vectors from the depth reprojected through the two cameras. The game has no motion vectors of its
//    own (no velocity target: the scene pass draws into one B10G11R11 target, and its upscale network samples the colour alone),
//    so that is the camera's motion, and what moves by itself would ghost. The scene camera's sub-pixel jitter
//    (nx2/oot_jitter.py, while an engine draws) is read back from its projection, given to the engine and taken off the motion.
//  - actor motion (actor_motion, shaders/dlss_actor.vert): the skinned draws into the scene (vk_draw.cpp's skin_check:
//    Link, the other characters, the animals, what the game animates with bones) are drawn again over that motion, each
//    vertex skinned with this frame's bones and the frame before's, so their pixels get their own motion. Their poses are
//    copied on the GPU from where the game has them into two buffers that take turns; a pixel is the draw's where its depth is
//    the scene's (its distance within RX_VK_DLSS_ACTOR_TOL thousandths of the scene's, default 10: 1%). What moves without bones (particles, water, foliage moved in
//    its vertex shader, mesh-shader draws) keeps the camera's motion. RX_VK_DLSS_ACTORS=0: camera motion alone.
//    That is the fallback now (RX_VK_DLSS_ACTORS=bones): by default every plain draw of triangles into the scene is drawn
//    again from the positions its own vertex stage stored as the game drew it (capture_motion, shaders/dlss_capture.vert):
//    the shader recompiler gives every plain vertex stage a store of its final position at a pushed address
//    (RuntimeInfo::capture_position; vk_draw.cpp push_capture asks temporal_capture_draw for a slot), into a buffer of the
//    frame's (a ring of kCaptureBuffers). The frame before's positions of the same draw (dlss_actor::key) give the motion,
//    exactly what the game's shader did: bones, waves along normals, morphs, rigid objects moving. The same depth test.
//  - the engine scales them to the output's size; shaders/dlss_out.comp squares that back to linear colour into the
//    network's output.
// Anything unexpected (no camera this frame, other sizes, the engine not there or failing) leaves the network to run, the
// reason in the log when it changes.
//
// The Insert menu's Upscaler row (opts.dlss, upscaler_rows.h) chooses the engine; RX_VK_DLSS=1 / 2 (DLSS, DLAA) or
// RX_VK_FSR4=1 / 2 (FSR 4, its native anti-aliasing) over it, and =0 takes that engine out for the run. AMD FSR 3.1
// (vk_fsr3.cpp, the row's value 3 or RX_VK_FSR=1) has inputs of its own (vk_fsr3_inputs.cpp): while it upscales, this chain
// stands aside.
// RX_VK_DLSS_DEBUG=motion: the motion vectors shown instead of the picture (|x|, |y| over 16 px as red and green, the actors'
// in it); =characters: a grey picture, blue where a draw moves by itself (not as the camera moves what is there: a still
// character is grey), brighter the further apart, up to 8 px; =alternate: every other frame the game's own upscale (a test's reference beside the engine's). (The names are
// DLSS's from before FSR came: they apply to either engine.)
#include "vk_temporal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "dlss_actor.h"
#include "dlss_checks.h"
#include "dlss_math.h"
#include "upscaler_rows.h"
#include "vk_dlss.h"
#include "vk_draw.h"
#include "vk_fsr3.h"   // (fsr3_wanted: AMD FSR 3.1 upscales with inputs of its own)
#include "vk_fsr4.h"
#include "vk_menu.h"
#include "vk_renodx.h"   // (renodx_built: temporal_scene_format)
#include "vk_texture.h"

#include "shaders/dlss_actor_frag_spv.h"
#include "shaders/dlss_capture_vert_spv.h"
#include "shaders/dlss_actor_vert_spv.h"
#include "shaders/dlss_merge_comp_spv.h"
#include "shaders/dlss_out_comp_spv.h"
#include "shaders/dlss_out_hdr_comp_spv.h"
#include "shaders/dlss_prep_hdr_comp_spv.h"
#include "shaders/dlss_prep_comp_spv.h"
#include "vk_work.h"   // (after the others: counts the commands that do GPU work, for full_barrier)

namespace mkwvk {

namespace ur = upscaler_rows;

namespace {
int env_switch(const char* name) {   // (-1: unset)
    const char* e = getenv(name);
    return e && *e ? atoi(e) : -1;
}
}  // namespace

namespace {
int env_dlss() { static const int v = env_switch("RX_VK_DLSS"); return v; }   // (read at the first ask, as the others)
int env_fsr4() { static const int v = env_switch("RX_VK_FSR4"); return v; }
}  // namespace

bool temporal_env_chosen() {
    // (only where the engine asked for runs: else a saved FSR 3.1 stays, not the game's own. DLSS before FSR 4, as below)
    if (env_dlss() > 0) return dlss_why() == DLSS_OK;
    return env_fsr4() > 0 && fsr4_why() == FSR_OK;
}

int temporal_mode() {
    const int dlss = env_dlss(), fsr4 = env_fsr4();
    int v = opts.dlss;
    if (dlss > 0) v = dlss >= 2 ? ur::DLAA : ur::DLSS;   // (2 and up: DLAA, as RX_VK_DLSS was clamped before)
    else if (fsr4 > 0) v = fsr4 >= 2 ? ur::FSR4_AA : ur::FSR4;
    // (FSR 3.1, the row's 3, is not an engine of this chain: the game's own as far as it is concerned. Nor anything while
    // FSR 3.1 upscales: one upscaler at a time, and one camera jitter. A switch above goes over the row's FSR 3.1 too
    // (fsr3_wanted asks temporal_env_chosen); RX_VK_FSR=1 goes over them)
    if (!ur::is_dlss(v) && !ur::is_fsr4(v)) return ur::GAME;
    if (fsr3_wanted()) return ur::GAME;
    if (ur::is_dlss(v) && dlss_why() != DLSS_OK) return ur::GAME;
    if (ur::is_fsr4(v) && fsr4_why() != FSR_OK) return ur::GAME;
    return v;
}
bool temporal_wanted() { return temporal_mode() != ur::GAME; }
namespace {
bool pre_wanted(int mode);   // (below: the engine, or AMD FSR 3.1, before the game's bloom and tone mapping)
}
bool temporal_full_scale() { return ur::full_scale(temporal_mode()); }

namespace {

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation memory = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
    TemporalImage shown() const { return {image, view, format, width, height}; }
};

struct Camera {
    float current[16]{};      // (its view-projection)
    float view[12]{}, proj[16]{};   // (its view, 3 x 4, and projection apart: the buffer's 0x00 and 0x70, row major)
    float jitter[2]{};        // (the projection's offset of the picture's centre, in clip space: nx2/oot_jitter.py)
    float near_z = 0, far_z = 0, fov_y = 0;   // (its projection's, 0 when it is not a plain perspective one)
    uint32_t width = 0, height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;   // (the scene's colour target's: for the log)
    const Texture* color = nullptr;          // (the scene's colour target itself: temporal_pre_tonemap draws into it)
    Texture* depth = nullptr;
    uint64_t depth_gen = 0;   // (texture_destroy_generation when taken: a depth texture destroyed since is not used)
    uint64_t frame = 0;       // (texture_frame_no when taken)
    bool valid = false;
};

/// An engine as the chain keeps it: started once, failed for good once
struct Engine {
    const TemporalEngine* e = nullptr;
    bool started = false, ok = false;
    std::atomic<bool> failed{false};   // (atomic: temporal_jitter_size reads it on the game's thread, without the lock)
};

struct State {
    std::mutex mu;
    Engine engines[2];               // (DLSS, FSR: engine_of)
    const TemporalEngine* drawing = nullptr;   // (the engine that drew last: another one starts a new history)
    Image color, depth, motion, output;
    // (the engine before the bloom, scaling up: the scene's colour target as its pass left it, in the scene's format; and
    // the engine's picture with what the game drew into the scene since: shaders/dlss_merge.comp)
    Image base, merged;
    VkDescriptorSetLayout merge_set = VK_NULL_HANDLE;
    VkPipelineLayout merge_layout = VK_NULL_HANDLE;
    VkPipeline merge = VK_NULL_HANDLE;
    bool pre_touched = false;   // (a draw into the scene since the engine's pass, this frame)
    // (the picture drawn before the bloom this frame, linear HDR: an engine's (output), or AMD FSR 3.1's own)
    struct {
        VkImageView view = VK_NULL_HANDLE;
        uint32_t width = 0, height = 0;
    } pre_picture;
    VkDescriptorSetLayout prep_set = VK_NULL_HANDLE, out_set = VK_NULL_HANDLE;
    VkPipelineLayout prep_layout = VK_NULL_HANDLE, out_layout = VK_NULL_HANDLE;
    // (local, RenoDX port) the same passes for an RGBA16F scene (the shaders' -DHDR): linear HDR colour in and out. The
    // plain ones' layouts; made at the first HDR frame
    VkPipeline prep_hdr = VK_NULL_HANDLE, out_hdr = VK_NULL_HANDLE;
    VkPipeline prep = VK_NULL_HANDLE, out = VK_NULL_HANDLE;
    VkSampler point = VK_NULL_HANDLE, linear = VK_NULL_HANDLE;
    Camera cam;               // (this frame's, since the last upscale)
    // (the engine before the game's bloom and tone mapping, temporal_pre_point: the program of the first draw after the
    // scene's last, as the frame before had it (the bloom's first pass); this frame's so far; whether the engine drew there
    // this frame, the upscale's dispatch still to come; the scene it drew into; draws into the scene after it)
    // (a draw as the point is known by: its program and its colour target 0's format and size. The pause screen draws the
    // bloom's program too, into other targets, in frames that have no upscale: not the point)
    struct PrePoint {
        uint64_t program = 0;
        VkFormat format = VK_FORMAT_UNDEFINED;
        uint32_t width = 0, height = 0;
        bool operator==(const PrePoint& o) const {
            return program == o.program && format == o.format && width == o.width && height == o.height;
        }
    };
    PrePoint pre_key, pre_candidate;
    bool pre_pending = false;
    uint64_t pre_frame = 0;   // (the frame the engine drew there: pending from an earlier frame had no upscale after it)
    // (after it, the game's tone mapping draw (temporal_pre_composite): the target it draws into, the picture before the
    // game's own edge anti-aliasing; whether it was drawn again into the upscale's output at the output's size, from the
    // engine's picture (an engine that scales up); that output as the upscale's dispatch last had it)
    const Texture* pre_final = nullptr;
    bool pre_direct = false;
    Texture* pre_out = nullptr;
    uint64_t pre_out_gen = 0;
    const Texture* pre_scene = nullptr;
    uint64_t pre_late = 0, pre_drawn = 0, pre_missed = 0;
    int pre_miss_run = 0;   // (frames in a row without the point)
    uint64_t pre_late_frame = 0;
    int pre_late_dumps = 0;
    // (the gameplay upscale's output, as last seen: the scene sizes the upscaler can take follow from it,
    // temporal_scene_size. Read on the game's draws without the lock)
    std::atomic<uint32_t> out_w{0}, out_h{0};
    std::atomic<uint64_t> last_frame{0};   // (the frame an engine last drew: read on the game's thread too)
    float last_jitter[2]{};   // (its camera's jitter: the frame before's, for the next one)
    float last_vp[16]{};      // (its camera's view-projection: the frame before's)
    std::atomic<uint32_t> jitter_w{0}, jitter_h{0};   // (the scene's size while an engine draws: the host's jitter's scale)
    std::atomic<uint32_t> jitter_phases{8};           // (its sequence's length, for the engine's scale: dlss_jitter_phases)
    std::unordered_map<VkImage, VkImageView> depth_views;
    uint64_t depth_views_gen = 0;
    const char* reason = nullptr;
    int reason_repeats = 0;
    const TemporalEngine* told_first = nullptr;
    bool told_camera = false;
    // (actor motion: the skinned draws into the scene since the last upscale; the poses the engine last drew, by draw: the
    // frame before's, where they were copied to in one of the two pose buffers (each frame's poses go to the other one))
    struct Skin {
        SkinnedDraw k;
        uint64_t key = 0;
        uint32_t nbones = 0;
        // (its pose as the draw had it, read when it was recorded: the origin (12 bytes, 4 more), then the bones. Empty: not
        // readable, copied on the GPU at the upscale instead)
        std::vector<uint8_t> pose;
    };
    std::vector<Skin> skins;
    struct Pose {
        uint32_t at = 0, nbones = 0;   // (bytes into its buffer: the origin, then the bones from at + 16)
        uint64_t frame = 0;
        int buf = 0;                   // (which of pose_buffers)
    };
    std::unordered_map<uint64_t, Pose> poses;
    std::unordered_map<uint64_t, uint32_t> model_bones;   // (by model: the bones its vertices use, 0 when unreadable)
    struct PoseBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation memory = nullptr;
        uint64_t address = 0;
        uint8_t* map = nullptr;
    } pose_buffers[6];          // (a ring, written by the CPU: one a frame, read that frame and the next, then left for frames
                                // still in flight, as uploads)
    int pose_at = 0;            // (this frame's)
    VkPipelineLayout actor_layout = VK_NULL_HANDLE;   // (the redraws': the bones' and the stored positions')
    VkPipeline actor = VK_NULL_HANDLE, capture = VK_NULL_HANDLE;
    bool actor_tried = false, capture_tried = false, layout_tried = false;
    // (capture motion: the draws that stored their positions since the last upscale, where (temporal_capture_draw); the
    // frame before's, by draw (dlss_actor::key), from the upscale at captured_frame; the buffers they are in, a ring, each a
    // frame's: written by the scene's draws, read at that frame's upscale and the next one's, then left for frames in flight)
    struct Captured {
        CaptureDraw k;
        uint64_t key = 0;
        uint64_t address = 0;
        uint32_t first = 0, count = 0;
        float jitter[2]{};      // (its own camera's, when own: dlss_actor::camera_jitter; else the scene's is used)
        bool own = false;
    };
    // (the camera read last for a stored draw, most draws of a frame having the same one)
    uint64_t capture_camera = 0, capture_camera_view = 0;
    float capture_camera_jitter[2]{};
    bool capture_camera_own = false;
    uint64_t capture_unjittered = 0;   // (stored draws whose own camera's jitter is not the scene's, since the last status line)
    std::vector<Captured> captured;
    std::unordered_map<uint64_t, int> captured_models;   // (by model: its draws so far this frame, for the key's nth)
    std::unordered_map<uint64_t, Captured> captured_before;
    uint64_t captured_frame = 0;
    struct CaptureBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation memory = nullptr;
        uint64_t address = 0, size = 0;
    } capture_buffers[4];
    int capture_at = 0;                // (this frame's)
    uint64_t capture_used = 0;         // (bytes of it given out)
    uint64_t capture_frame = 0;        // (the frame its first draw was recorded in: stale once that is over a frame ago)
    uint64_t capture_want = 0;         // (the size the next ones are made at: grown when a frame ran out)
    bool capture_full = false;         // (this frame's ran out: a draw was given no slot)
    int capture_ok = -1;               // (buffer device addresses and 64-bit integers in shaders: -1 not asked yet)
    // (by index buffer, count and type: the least and greatest index, read once (dlss_actor::index_range); unreadable: none)
    struct IndexRange { uint32_t lo = 0, hi = 0; bool ok = false; };
    std::unordered_map<uint64_t, IndexRange> index_ranges;
    uint64_t capture_slots = 0, capture_draws = 0, capture_bytes_peak = 0;   // (since the last status line)
    // (a diagnostic, RX_VK_DLSS_PROBE=1: the first positions of the frame's largest stored draw, this frame's and the frame
    // before's, copied back and compared a few frames later with the camera's motion of the same vertices, in the log)
    struct Probe {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation memory = nullptr;
        void* map = nullptr;
        uint64_t frame = 0, key = 0;
        uint32_t count = 0;
        float reproject[16]{}, jitter_now[2]{}, jitter_before[2]{}, size[2]{};
        bool used = false;
    } probes[8];
    int probe_at = 0, probes_told = 0;
    struct Upload {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation memory = nullptr;
        void* map = nullptr;
    } uploads[6];               // (the cameras: a ring, one in use stays so for a few frames, as what later() frees)
    int upload_at = 0;
    uint64_t actor_draws = 0, actor_frames = 0, actor_skipped = 0;
    int64_t actor_ns = 0;       // (the CPU's time in it, since the last status line)
    uint64_t status_draws = 0, status_frames = 0;   // (actor_draws and _frames at the last status line: temporal_status)
    uint64_t status_drawn = 0, status_passed = 0;   // (since the last status line: frames an engine drew, and frames a network
                                                    // of the upscale's shape ran without it (its scene not this frame's))
    uint64_t engines_made = 0;  // (prepare's new histories: a feature or context made again, each a stall in play)
    float status_jitter[2]{};   // (the last evaluation's jitter, in render pixels, and whether it had motion)
    bool status_motion = false;
    struct Dead { uint64_t frame; std::function<void()> free; };
    std::vector<Dead> dead;   // (what work in flight may still use: freed a few frames on)
    uint64_t asked = 0;       // (the frame an upscale last asked for an engine: temporal_idle)
    std::atomic<bool> holding{false};   // (an engine's state, images or frees still to do: temporal_idle has something to let go)
};
State& st() {
    // (never destroyed: Hookshot leaves through ExitProcess / exit() with the device alive, and `dead` may still hold an
    // engine's state then (an FSR context), whose destructor would call into a library already gone)
    static State& s = *new State;
    return s;
}

/// The gameplay upscale's output size, as it is seen: the scene sizes the upscaler can take (temporal_scene_size). Until the
/// first one, none: that frame keeps the game's upscale
void note_output(const Texture* out, uint32_t level) {
    State& s = st();
    s.out_w.store(std::max(1u, out->width >> level), std::memory_order_relaxed);
    s.out_h.store(std::max(1u, out->height >> level), std::memory_order_relaxed);
}

/// The chain's slot for the engine of an Upscaler value (DLSS and DLAA: DLSS's; FSR 4 and its native AA: FSR's)
Engine& engine_of(int mode) {
    State& s = st();
    Engine& k = s.engines[ur::is_fsr4(mode) ? 1 : 0];
    if (!k.e) k.e = ur::is_fsr4(mode) ? fsr4_engine() : dlss_engine();
    return k;
}
const char* name_of(int mode) {
    switch (mode) {
    case ur::DLSS: return "NVIDIA DLSS";
    case ur::DLAA: return "NVIDIA DLAA";
    case ur::FSR4: return "AMD FSR 4";
    case ur::FSR4_AA: return "AMD FSR 4 Native AA";
    default: return "the game's own";
    }
}

bool fail(const char* why) {   // (the network runs: why, when it changes and every 600th time)
    State& s = st();
    if (why != s.reason || ++s.reason_repeats % 600 == 0)
        logf("vk: %s: the game's upscale runs (%s)", name_of(temporal_mode()), why);
    if (why != s.reason) s.reason = why, s.reason_repeats = 0;
    return false;
}

void later(std::function<void()> f) { st().dead.push_back({texture_frame_no(), std::move(f)}); }
void free_dead(bool all) {
    State& s = st();
    const uint64_t now = texture_frame_no();
    for (size_t i = 0; i < s.dead.size();) {
        if (all || now - s.dead[i].frame > 4) {
            s.dead[i].free();
            s.dead[i] = std::move(s.dead.back());
            s.dead.pop_back();
        } else {
            i++;
        }
    }
}

void drop_image(Image& im) {
    if (!im.image) return;
    const Image old = im;
    later([old] {
        vkDestroyImageView(dev.device, old.view, nullptr);
        destroy_image(old.image, old.memory);
    });
    im = {};
}

bool make_image(VkCommandBuffer cmd, Image& im, VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags extra = 0) {
    if (im.image && im.format == format && im.width == w && im.height == h) return true;
    drop_image(im);
    VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ic.imageType = VK_IMAGE_TYPE_2D, ic.format = format, ic.extent = {w, h, 1}, ic.mipLevels = 1, ic.arrayLayers = 1;
    ic.samples = VK_SAMPLE_COUNT_1_BIT, ic.tiling = VK_IMAGE_TILING_OPTIMAL;
    ic.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | extra;
    if (!create_image(ic, im.image, im.memory)) return im = {}, false;
    VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vc.image = im.image, vc.viewType = VK_IMAGE_VIEW_TYPE_2D, vc.format = format;
    vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(dev.device, &vc, nullptr, &im.view) != VK_SUCCESS) {
        destroy_image(im.image, im.memory);
        return im = {}, false;
    }
    im.format = format, im.width = w, im.height = h;
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};   // (UNDEFINED -> GENERAL, once)
    b.srcStageMask = VK_PIPELINE_STAGE_2_NONE, b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im.image, b.subresourceRange = vc.subresourceRange;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1, di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
    return true;
}

/// One compute pipeline of a layout; pipe stays null when it fails
bool make_compute(const uint32_t* code, size_t size, VkPipelineLayout layout, VkPipeline& pipe) {
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = size, mi.pCode = code;
    VkShaderModule m{};
    if (vkCreateShaderModule(dev.device, &mi, nullptr, &m) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", nullptr};
    ci.layout = layout;
    const bool ok = vkCreateComputePipelines(dev.device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipe) == VK_SUCCESS;
    vkDestroyShaderModule(dev.device, m, nullptr);
    if (!ok) pipe = VK_NULL_HANDLE;
    return ok;
}

/// One compute pipeline with push descriptors; what it made is destroyed again when a step fails
bool make_pipeline(const uint32_t* code, size_t size, const VkDescriptorType* types, uint32_t n, uint32_t push,
                   VkDescriptorSetLayout& set, VkPipelineLayout& layout, VkPipeline& pipe) {
    const auto undo = [&] {
        if (layout) vkDestroyPipelineLayout(dev.device, layout, nullptr);
        if (set) vkDestroyDescriptorSetLayout(dev.device, set, nullptr);
        set = VK_NULL_HANDLE, layout = VK_NULL_HANDLE, pipe = VK_NULL_HANDLE;
        return false;
    };
    VkDescriptorSetLayoutBinding b[8]{};
    for (uint32_t i = 0; i < n; i++) b[i] = {i, types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = n, li.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev.device, &li, nullptr, &set) != VK_SUCCESS) return set = VK_NULL_HANDLE, undo();
    const VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, push};
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1, pi.pSetLayouts = &set, pi.pushConstantRangeCount = 1, pi.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(dev.device, &pi, nullptr, &layout) != VK_SUCCESS) return layout = VK_NULL_HANDLE, undo();
    return make_compute(code, size, layout, pipe) || undo();
}

/// The two passes' pipelines and the samplers, once (temporal_upscale stops trying when this fails). hdr: their HDR
/// variants too, which an RGBA16F scene draws with
bool make_pipelines(bool hdr) {
    State& s = st();
    if (s.prep && s.out && s.point && s.linear && (!hdr || (s.prep_hdr && s.out_hdr))) return true;
    if (!dev.push_descriptor || !vkCmdPushDescriptorSetKHR) return false;
    const VkDescriptorType S = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, I = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    const VkDescriptorType prep_types[5] = {S, S, I, I, I}, out_types[2] = {S, I};
    if (!s.prep && !make_pipeline(DLSS_PREP_COMP_SPV, sizeof DLSS_PREP_COMP_SPV, prep_types, 5, 84, s.prep_set, s.prep_layout, s.prep))
        return false;
    if (!s.out && !make_pipeline(DLSS_OUT_COMP_SPV, sizeof DLSS_OUT_COMP_SPV, out_types, 2, 12, s.out_set, s.out_layout, s.out))
        return false;
    if (hdr && !s.prep_hdr && !make_compute(DLSS_PREP_HDR_COMP_SPV, sizeof DLSS_PREP_HDR_COMP_SPV, s.prep_layout, s.prep_hdr))
        return false;
    if (hdr && !s.out_hdr && !make_compute(DLSS_OUT_HDR_COMP_SPV, sizeof DLSS_OUT_HDR_COMP_SPV, s.out_layout, s.out_hdr))
        return false;
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (!s.point && vkCreateSampler(dev.device, &si, nullptr, &s.point) != VK_SUCCESS) return s.point = VK_NULL_HANDLE, false;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    if (!s.linear && vkCreateSampler(dev.device, &si, nullptr, &s.linear) != VK_SUCCESS) return s.linear = VK_NULL_HANDLE, false;
    return true;
}

/// A view of a depth texture's depth alone, for sampling (made once per image); of a colour texture, its colour (the
/// scene's target, sampled and written by temporal_pre_tonemap)
VkImageView depth_view(Texture* t) {
    State& s = st();
    const uint64_t gen = texture_destroy_generation();
    if (gen != s.depth_views_gen) {   // (a texture was destroyed: its image's view may be stale)
        auto old = std::move(s.depth_views);
        s.depth_views.clear();
        later([old] {
            for (const auto& [image, view] : old) vkDestroyImageView(dev.device, view, nullptr);
        });
        s.depth_views_gen = gen;
    }
    if (auto it = s.depth_views.find(t->image); it != s.depth_views.end()) return it->second;
    VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vc.image = t->image, vc.viewType = VK_IMAGE_VIEW_TYPE_2D, vc.format = t->format;
    vc.subresourceRange = {(t->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT,
                           0, 1, 0, 1};
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(dev.device, &vc, nullptr, &v) != VK_SUCCESS) v = VK_NULL_HANDLE;
    s.depth_views[t->image] = v;
    return v;
}

// ---- actor motion: the skinned draws drawn again into the motion, each pixel they show moved by its own bones ----

/// RX_VK_DLSS_ACTORS: what gives what moves by itself its own motion. 0: nothing (the camera's motion alone); bones (or 2):
/// the skinned draws from their bones (actor_motion), as before; else (unset, 1) every draw from the positions its vertex
/// stage stored (capture_motion), or the bones when the stages store none (RX_VK_DLSS_CAPTURE=0, vk_shader.cpp)
enum MotionMode { kMotionCamera = 0, kMotionCapture = 1, kMotionBones = 2 };
std::atomic<uint64_t> bench_upscales{0};   // (the upscales an engine drew: RX_VK_DLSS_BENCH's clock)
constexpr uint64_t kBenchFrames = 300;     // (a mode's turn)
MotionMode motion_mode() {
    // (RX_VK_DLSS_BENCH=1, a measurement: the three take turns of kBenchFrames upscales each, the camera's motion alone, the
    // bones, the stored positions, and each turn's frame time is logged: temporal_upscale)
    static const bool bench = env_switch("RX_VK_DLSS_BENCH") > 0;
    if (bench) {
        static const MotionMode turns[3] = {kMotionCamera, kMotionBones, kMotionCapture};
        return turns[(bench_upscales.load(std::memory_order_relaxed) / kBenchFrames) % 3];
    }
    static const MotionMode m = [] {
        const char* e = getenv("RX_VK_DLSS_ACTORS");
        const char* c = getenv("RX_VK_DLSS_CAPTURE");
        const bool stored = !c || atoi(c) != 0;
        if (e && (!strcmp(e, "bones") || !strcmp(e, "2"))) return kMotionBones;
        if (e && *e && atoi(e) == 0) return kMotionCamera;
        return stored ? kMotionCapture : kMotionBones;
    }();
    return m;
}

using ActorPush = dlss_actor::Push;

/// The redraws' layout, once: the cameras' buffer and the scene's depth (pushed), and the vertex stage's push constants (the
/// bones' Push, the stored positions' CapturePush in its first bytes). False: none, the motion stays the camera's
bool make_redraw_layout() {
    State& s = st();
    if (s.actor_layout) return true;
    if (s.layout_tried) return false;
    s.layout_tried = true;
    VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    vkGetPhysicalDeviceFeatures2(dev.physical, &f);
    if (!dev.vk12.bufferDeviceAddress || !f.features.shaderInt64) {   // (its shaders read the game's buffers by their addresses)
        logf("vk: upscaler: no buffer device addresses or 64-bit integers in shaders: camera motion alone");
        return false;
    }
    VkDescriptorSetLayoutBinding b[2]{};
    b[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    b[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = 2, li.pBindings = b;
    VkDescriptorSetLayout set{};
    if (vkCreateDescriptorSetLayout(dev.device, &li, nullptr, &set) != VK_SUCCESS) return false;
    static_assert(sizeof(ActorPush) >= sizeof(dlss_actor::CapturePush), "the redraws' push constants");
    const VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ActorPush)};
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1, pi.pSetLayouts = &set, pi.pushConstantRangeCount = 1, pi.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(dev.device, &pi, nullptr, &s.actor_layout) != VK_SUCCESS) return s.actor_layout = VK_NULL_HANDLE, false;
    return true;
}

/// A redraw's pipeline: a vertex stage, then dlss_actor.frag into the motion (RG16F). per_draw: the topology and primitive
/// restart set for each draw, as the game drew it (the stored positions'; the bones' draw triangle lists)
VkPipeline make_redraw(const uint32_t* vcode, size_t vsize, bool per_draw) {
    State& s = st();
    VkShaderModule m[2]{};
    const uint32_t* code[2] = {vcode, DLSS_ACTOR_FRAG_SPV};
    const size_t size[2] = {vsize, sizeof DLSS_ACTOR_FRAG_SPV};
    for (int i = 0; i < 2; i++) {
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = size[i], mi.pCode = code[i];
        if (vkCreateShaderModule(dev.device, &mi, nullptr, &m[i]) != VK_SUCCESS) {
            if (i) vkDestroyShaderModule(dev.device, m[0], nullptr);
            return VK_NULL_HANDLE;
        }
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (int i = 0; i < 2; i++) {
        stages[i] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
        stages[i].module = m[i], stages[i].pName = "main";
    }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL, rs.cullMode = VK_CULL_MODE_NONE, rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba{};
    ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1, cb.pAttachments = &ba;
    const VkDynamicState dyn[4] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
                                   VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = per_draw ? 4 : 2, ds.pDynamicStates = dyn;
    const VkFormat format = VK_FORMAT_R16G16_SFLOAT;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = 1, ri.pColorAttachmentFormats = &format;
    VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.pNext = &ri;
    gi.stageCount = 2, gi.pStages = stages;
    gi.pVertexInputState = &vi, gi.pInputAssemblyState = &ia, gi.pViewportState = &vp, gi.pRasterizationState = &rs;
    gi.pMultisampleState = &ms, gi.pColorBlendState = &cb, gi.pDynamicState = &ds, gi.layout = s.actor_layout;
    VkPipeline pipe = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(dev.device, VK_NULL_HANDLE, 1, &gi, nullptr, &pipe) != VK_SUCCESS) pipe = VK_NULL_HANDLE;
    for (VkShaderModule x : m) vkDestroyShaderModule(dev.device, x, nullptr);
    return pipe;
}

/// The bones' redraw pipeline, once (false: none, the motion stays the camera's)
bool make_actor_pipeline() {
    State& s = st();
    if (s.actor) return true;
    if (s.actor_tried) return false;
    s.actor_tried = true;
    if (!make_redraw_layout()) return false;
    s.actor = make_redraw(DLSS_ACTOR_VERT_SPV, sizeof DLSS_ACTOR_VERT_SPV, false);
    if (!s.actor) logf("vk: upscaler: the actor motion's pipeline was not made: camera motion alone");
    return s.actor != VK_NULL_HANDLE;
}

/// The stored positions' redraw pipeline, once (false: none)
bool make_capture_pipeline() {
    State& s = st();
    if (s.capture) return true;
    if (s.capture_tried) return false;
    s.capture_tried = true;
    if (!make_redraw_layout()) return false;
    s.capture = make_redraw(DLSS_CAPTURE_VERT_SPV, sizeof DLSS_CAPTURE_VERT_SPV, true);
    if (!s.capture) logf("vk: upscaler: the captured motion's pipeline was not made: camera motion alone");
    return s.capture != VK_NULL_HANDLE;
}

/// A range of guest memory as this recording binds it, by its device address (the pools and their mirrors have them)
uint64_t device_address(const BufferRef& r) {
    if (!r) return 0;
    VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    da.buffer = r.buffer;
    const VkDeviceAddress base = vkGetBufferDeviceAddress(dev.device, &da);
    return base ? base + r.offset : 0;
}

/// The redraws' tolerance word (dlss_actor.h kToleranceWord): a pixel is the draw's where its distance from the camera is
/// within this many thousandths of the scene's there (1%; RX_VK_DLSS_ACTOR_TOL). In depth buffer steps (64) as before, the
/// redrawn characters met the scene in stripes, rounding apart; in distance a character is still told from a wall behind
/// it. Without a plain perspective projection: 64 steps. RX_VK_DLSS_DEBUG=actors: the pixels given their own motion marked
uint32_t redraw_tolerance() {
    static const uint32_t tolerance = env_switch("RX_VK_DLSS_ACTOR_TOL") >= 0 ? (uint32_t)env_switch("RX_VK_DLSS_ACTOR_TOL") : 10;
    static const char* const debug = getenv("RX_VK_DLSS_DEBUG");
    static const bool own = debug && !strcmp(debug, "characters");   // (=characters: the marks alone, on grey)
    static const bool mark = own || (debug && !strcmp(debug, "actors"));
    return (tolerance & ~(dlss_actor::kMarkBit | dlss_actor::kOwnBit)) | (mark ? dlss_actor::kMarkBit : 0u) |
           (own ? dlss_actor::kOwnBit : 0u);
}

/// A redraw into the motion begun: the cameras' buffer written (this frame's as drawn, the frame before's without its jitter
/// (before), this frame's jitter, the render size, the view and projection, the planes, the tolerance, the frame before's
/// jitter (jitter_before: the stored positions'; null none), the prep pass's reproject), pipe bound with it and the scene's depth, the motion the
/// target. False: nothing begun
bool begin_redraw(VkCommandBuffer cmd, VkPipeline pipe, const Camera& cam, const float* before, const float* jitter_before,
                  const float* reproject, uint32_t rw, uint32_t rh, VkImageView dview) {
    State& s = st();
    State::Upload& u = s.uploads[s.upload_at];
    s.upload_at = (s.upload_at + 1) % (int)(sizeof s.uploads / sizeof s.uploads[0]);
    if (!u.buffer && !create_buffer_cached(dlss_actor::kHeadWords * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, u.buffer, u.memory, &u.map))
        return u = {}, false;
    float* head = (float*)u.map;
    std::memset(head, 0, dlss_actor::kHeadWords * 4);
    std::memcpy(head, cam.current, 64);
    std::memcpy(head + 16, before, 64);
    head[32] = cam.jitter[0], head[33] = cam.jitter[1], head[34] = (float)rw, head[35] = (float)rh;
    std::memcpy(head + 36, cam.view, sizeof cam.view), std::memcpy(head + 48, cam.proj, sizeof cam.proj);
    head[64] = cam.near_z, head[65] = cam.far_z;
    const uint32_t tolerance = redraw_tolerance();
    std::memcpy(head + dlss_actor::kToleranceWord, &tolerance, 4);
    if (jitter_before) head[dlss_actor::kJitterBeforeWord] = jitter_before[0], head[dlss_actor::kJitterBeforeWord + 1] = jitter_before[1];
    // (this frame's clip space to the frame before's, as the prep pass has it: the mark's comparison)
    std::memcpy(head + dlss_actor::kReprojectWord, reproject, 64);
    flush_mapped(u.memory, dlss_actor::kHeadWords * 4);
    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = s.motion.view, ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {rw, rh}}, ri.layerCount = 1, ri.colorAttachmentCount = 1, ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cmd, &ri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    const VkViewport viewport{0, 0, (float)rw, (float)rh, 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, {rw, rh}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    const VkDescriptorBufferInfo bi{u.buffer, 0, dlss_actor::kHeadWords * 4};
    const VkDescriptorImageInfo ii{s.point, dview, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[2]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    w[0].dstBinding = 0, w[0].descriptorCount = 1, w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, w[0].pBufferInfo = &bi;
    w[1].dstBinding = 1, w[1].descriptorCount = 1, w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[1].pImageInfo = &ii;
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.actor_layout, 0, 2, w);
    return true;
}

constexpr uint64_t kPoseBytes = 4 << 20;   // (a pose buffer: 280 bones of 48 bytes are 13 KB a draw, 300 of them)

/// The skinned draws into the scene (cam's depth) with a pose of the frame before (the engine's last), drawn into the motion
/// image over the camera's motion. Each one's bones and origin, as the draw had them (read when it was recorded:
/// temporal_note_skinned_draw), go into this frame's pose buffer, for this frame and the next. They were copied on the GPU
/// at the upscale before, from where the game has them; by then the game could have written the next frame's pose there,
/// and the limbs that moved most were drawn where the scene was not: no motion of their own. before: the frame before's camera without its jitter. history:
/// the engine has the frame before. Returns the draws drawn
int actor_motion(VkCommandBuffer cmd, const Camera& cam, const float* before, const float* reproject, uint32_t rw,
                 uint32_t rh, VkImageView dview, bool history) {
    State& s = st();
    struct Timed {
        State& s;
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        ~Timed() { s.actor_ns += (std::chrono::steady_clock::now() - t0).count(); }
    } timed{s};
    std::vector<State::Skin> skins = std::move(s.skins);
    s.skins.clear();
    std::unordered_map<uint64_t, State::Pose> old = std::move(s.poses);
    s.poses.clear();
    if (motion_mode() != kMotionBones || skins.empty() || !make_actor_pipeline()) return 0;
    for (auto& b : s.pose_buffers)
        if (!b.buffer) {
            void* map = nullptr;
            if (!create_buffer_cached(kPoseBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, b.buffer, b.memory, &map) || !map)
                return b = {}, 0;
            b.map = (uint8_t*)map;
            b.address = device_address({b.buffer, 0, kPoseBytes, 0});
        }
    const uint64_t now = texture_frame_no();
    const State::PoseBuffer& here = s.pose_buffers[s.pose_at];
    std::vector<ActorPush> draws;
    std::vector<uint32_t> counts;
    uint32_t at = 0;
    // (RX_VK_DLSS_TRACE=<frames>: on vk_draw.cpp's traced frames, each skinned draw left out here and why)
    static const int trace_every = env_switch("RX_VK_DLSS_TRACE");
    const bool trace = trace_every > 0 && now % (uint64_t)trace_every == (uint64_t)trace_every / 2;
    const auto left = [&](const State::Skin& k, const char* why) {
        if (trace) logf("vk: dlss-trace actor left out: prog %016llx vb %llx n%d bones %llx: %s", (unsigned long long)k.k.program,
                        (unsigned long long)k.k.vbuf, k.k.count, (unsigned long long)k.k.bones, why);
    };
    for (const auto& k : skins) {
        if (k.k.depth != cam.depth) {
            left(k, "another depth target than the scene's");
            continue;
        }
        const uint32_t bytes = dlss_actor::kPoseHead + k.nbones * 48;
        if (at + bytes > kPoseBytes) {
            left(k, "the pose buffer is full");
            break;
        }
        // (this frame's pose: as the draw had it; else from where the game has it now, its bones and its origin at the
        // object's 0x40)
        if (k.pose.size() == bytes) {
            std::memcpy(here.map + at, k.pose.data(), bytes);
            // (said a few times a run: a draw whose camera is not the scene's, which the redraw follows now)
            static int told_camera = 0;
            if (told_camera < 4 && (std::memcmp(k.pose.data() + 64, cam.proj, sizeof cam.proj) ||
                                    std::memcmp(k.pose.data() + 16, cam.view, sizeof cam.view))) {
                told_camera++;
                const float* p = (const float*)(k.pose.data() + 64);
                logf("vk: upscaler: actor %016llx drawn with another camera than the scene's: its projection z row %g %g %g %g, "
                     "the scene's %g %g %g %g%s", (unsigned long long)k.k.program, p[8], p[9], p[10], p[11], cam.proj[8],
                     cam.proj[9], cam.proj[10], cam.proj[11],
                     std::memcmp(k.pose.data() + 16, cam.view, sizeof cam.view) ? " (its view too)" : "");
            }
        } else {
            const BufferRef bones = gpu_buffer_bind(k.k.bones, (uint64_t)k.nbones * 48, false);
            const BufferRef origin = gpu_buffer_bind(k.k.object + 0x40, 12, false);
            if (!bones || !origin) {
                s.actor_skipped++;
                left(k, "its bones or origin are not bound");
                continue;
            }
            const VkBufferCopy c0{origin.offset, at, 12}, c1{bones.offset, at + dlss_actor::kPoseHead, (uint64_t)k.nbones * 48};
            vkCmdCopyBuffer(cmd, origin.buffer, here.buffer, 1, &c0);
            vkCmdCopyBuffer(cmd, bones.buffer, here.buffer, 1, &c1);
            // (the camera: the scene's, as before)
            std::memcpy(here.map + at + 16, cam.view, sizeof cam.view), std::memcpy(here.map + at + 64, cam.proj, sizeof cam.proj);
        }
        s.poses[k.key] = {at, k.nbones, now, s.pose_at};
        const uint32_t mine = at;
        at += (bytes + 15) & ~15u;
        const auto it = old.find(k.key);
        if (!history || it == old.end() || it->second.frame != s.last_frame || it->second.nbones != k.nbones) {
            s.actor_skipped++;
            left(k, !history ? "no history" : it == old.end() ? "not drawn the frame before (its key)"
                    : it->second.frame != s.last_frame ? "its pose is older than the frame before" : "another bone count the frame before");
            continue;
        }
        // (the vertices and indices where the game has them: its buffers as this recording binds them)
        const uint64_t va = device_address(gpu_buffer_bind(k.k.vbuf, k.k.vsize, false));
        const bool arrays = k.k.itype == dlss_actor::kNoIndices;   // (no indices: a plain vertex array)
        const uint64_t ia = arrays ? 0 : device_address(gpu_buffer_bind(k.k.ibuf, (uint64_t)k.k.count << k.k.itype, false));
        if (!va || (!arrays && !ia) || va % 4 || k.k.count <= 0 || k.k.itype < 0 || k.k.itype > dlss_actor::kNoIndices) {
            s.actor_skipped++;
            left(k, "its vertices or indices are not bound");
            continue;
        }
        ActorPush a{};
        a.vertices = va, a.count = (uint32_t)std::min<uint64_t>(k.k.vsize / k.k.stride, UINT32_MAX);
        if (arrays) a.indices = 0, a.first = 0;
        else dlss_actor::index_start(ia, k.k.itype, &a.indices, &a.first);
        a.origins[0] = here.address + mine, a.origins[1] = s.pose_buffers[it->second.buf].address + it->second.at;
        a.bones[0] = a.origins[0] + dlss_actor::kPoseHead, a.bones[1] = a.origins[1] + dlss_actor::kPoseHead;
        a.nbones = k.nbones;
        a.itype = (uint32_t)k.k.itype, a.base_vertex = k.k.base_vertex;
        a.stride = k.k.stride / 4, a.pos = k.k.pos, a.sets = (uint32_t)k.k.sets;
        for (int i = 0; i < 2; i++) a.joints[i] = k.k.joints[i], a.weights[i] = k.k.weights[i];
        a.formats = k.k.formats;
        a.tolerance = redraw_tolerance();   // (dlss_actor.frag reads it from the cameras' buffer: begin_redraw)
        draws.push_back(a);
        counts.push_back((uint32_t)k.k.count);
    }
    if (at) flush_mapped_range(here.memory, 0, at);   // (the poses written here, for the GPU)
    s.pose_at = (s.pose_at + 1) % (int)(sizeof s.pose_buffers / sizeof s.pose_buffers[0]);
    if (draws.empty()) return 0;
    full_barrier(cmd);   // (the poses copied)
    if (!begin_redraw(cmd, s.actor, cam, before, nullptr, reproject, rw, rh, dview)) return 0;
    for (size_t i = 0; i < draws.size(); i++) {   // (vertex i of a draw is its i-th index)
        vkCmdPushConstants(cmd, s.actor_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof draws[i], &draws[i]);
        vkCmdDraw(cmd, counts[i], 1, 0, 0);
    }
    vkCmdEndRendering(cmd);
    s.actor_draws += draws.size(), s.actor_frames++;
    return (int)draws.size();
}

// ---- capture motion: every draw into the scene drawn again from the positions its vertex stage stored ----

constexpr int kCaptureBuffers = (int)(sizeof(State::capture_buffers) / sizeof(State::CaptureBuffer));
constexpr uint64_t kCaptureMost = 256ull << 20;     // (a frame's buffer grows to this at most)
constexpr uint32_t kCaptureMostSlots = 1u << 20;    // (a draw's)

/// This frame's stored draws, taken at its upscale; the ring moves on (the next frame's go into the next buffer, made again
/// larger when this one ran out)
std::vector<State::Captured> take_captures() {
    State& s = st();
    std::vector<State::Captured> c = std::move(s.captured);
    s.captured.clear(), s.captured_models.clear();
    s.capture_bytes_peak = std::max(s.capture_bytes_peak, s.capture_used);
    s.capture_at = (s.capture_at + 1) % kCaptureBuffers;
    s.capture_used = 0;
    if (s.capture_full && s.capture_want && s.capture_want < kCaptureMost) {
        s.capture_want = std::min(s.capture_want * 2, kCaptureMost);
        logf("vk: upscaler: a frame's draws stored more positions than their buffer holds: %llu MB from now on",
             (unsigned long long)(s.capture_want >> 20));
    }
    s.capture_full = false;
    State::CaptureBuffer& next = s.capture_buffers[s.capture_at];
    if (next.buffer && next.size < s.capture_want) {   // (a few frames old: let go, made again at the size wanted)
        const State::CaptureBuffer gone = next;
        later([gone] { destroy_buffer(gone.buffer, gone.memory); });
        next = {};
    }
    return c;
}

/// The draws that stored their positions this frame (caps) drawn again into the motion over the camera's, where they were
/// drawn the frame before too (the same draw: dlss_actor::key), from those positions: their own motion, whatever moved
/// them. before: the frame before's camera without its jitter; history: the engine has the frame before. Returns the
/// draws drawn
int capture_motion(VkCommandBuffer cmd, const Camera& cam, const float* before, const float* reproject, uint32_t rw,
                   uint32_t rh, VkImageView dview, bool history, const std::vector<State::Captured>& caps) {
    State& s = st();
    struct Timed {
        State& s;
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        ~Timed() { s.actor_ns += (std::chrono::steady_clock::now() - t0).count(); }
    } timed{s};
    std::unordered_map<uint64_t, State::Captured> old = std::move(s.captured_before);
    s.captured_before.clear();
    // (the frame before's are of the frame the engine last drew: else a gap, and their positions are older)
    const bool old_ok = history && s.captured_frame && s.captured_frame == s.last_frame;
    s.captured_frame = texture_frame_no();
    for (const auto& c : caps)
        if (c.k.depth == cam.depth) s.captured_before[c.key] = c;
    // (RX_VK_DLSS_TRACE=<frames>: on vk_draw.cpp's traced frames, each stored draw left out here and why)
    static const int trace_every = env_switch("RX_VK_DLSS_TRACE");
    const uint64_t now = texture_frame_no();
    const bool trace = trace_every > 0 && now % (uint64_t)trace_every == (uint64_t)trace_every / 2;
    const auto left = [&](const State::Captured& c, const char* why) {
        if (trace) logf("vk: dlss-trace capture left out: prog %016llx vb %llx ib %llx n%d: %s", (unsigned long long)c.k.program,
                        (unsigned long long)c.k.vbuf, (unsigned long long)c.k.ibuf, c.k.count, why);
    };
    if (caps.empty() || !make_capture_pipeline()) return 0;
    struct Redraw {
        dlss_actor::CapturePush push;
        const State::Captured* c;
        BufferRef ib;
    };
    std::vector<Redraw> draws;
    draws.reserve(caps.size());
    for (const auto& c : caps) {
        if (c.k.depth != cam.depth) {
            left(c, "another depth target than the scene's");
            continue;
        }
        const auto it = old_ok ? old.find(c.key) : old.end();
        if (it == old.end()) {
            s.actor_skipped++;
            left(c, !old_ok ? "no history" : "not drawn the frame before (its key)");
            continue;
        }
        // (its indices where the game has them, as this recording binds them: the same as it drew with)
        BufferRef ib{};
        if (c.k.itype != dlss_actor::kNoIndices) {
            ib = gpu_buffer_bind(c.k.ibuf, (uint64_t)c.k.count << c.k.itype, false);
            if (!ib) {
                s.actor_skipped++;
                left(c, "its indices are not bound");
                continue;
            }
        }
        const State::Captured& o = it->second;
        dlss_actor::CapturePush push{c.address, o.address, c.first, o.first, c.count, o.count, {}, {}};
        for (int i = 0; i < 2; i++) {
            push.jitter_now[i] = c.own ? c.jitter[i] : cam.jitter[i];
            push.jitter_before[i] = o.own ? o.jitter[i] : s.last_jitter[i];
        }
        if (c.own && (c.jitter[0] != cam.jitter[0] || c.jitter[1] != cam.jitter[1])) s.capture_unjittered++;
        draws.push_back({push, &c, ib});
    }
    if (draws.empty()) return 0;
    static const bool probe_on = env_switch("RX_VK_DLSS_PROBE") > 0;
    constexpr uint32_t kProbeSlots = 256;
    if (probe_on && s.probes_told < 300) {
        State::Probe& pr = s.probes[s.probe_at];
        s.probe_at = (s.probe_at + 1) % 8;
        if (pr.used) {   // (eight frames old: the GPU is past it)
            // (each vertex: its own motion, from where it is to where it was, and the camera's, its position now through the
            // reprojection, both in render pixels as the passes make them)
            const float* v = (const float*)pr.map;
            double most = 0.0, sum = 0.0, moved = 0.0;
            double worst[4]{};
            int n = 0;
            for (uint32_t i = 0; i < pr.count; i++) {
                const float* a = v + i * 4;
                const float* b = v + (kProbeSlots + i) * 4;
                if (!(a[3] > 0.2f) || !(b[3] > 0.2f)) continue;
                const double nx = a[0] / a[3], ny = a[1] / a[3];
                if (std::fabs(nx) > 1.0 || std::fabs(ny) > 1.0) continue;   // (on screen)
                const double clip[4] = {a[0], a[1], 2.0 * a[2] - a[3], a[3]};   // (depth -1 to 1 again)
                double was[4];
                for (int r = 0; r < 4; r++) {
                    was[r] = 0.0;
                    for (int c = 0; c < 4; c++) was[r] += (double)pr.reproject[r * 4 + c] * clip[c];
                }
                if (!(was[3] > 1e-6)) continue;
                const double now_x = nx - pr.jitter_now[0], now_y = ny - pr.jitter_now[1];
                const double own_x = ((b[0] - pr.jitter_before[0] * b[3]) / b[3] - now_x) * 0.5 * pr.size[0];
                const double own_y = (now_y - (b[1] - pr.jitter_before[1] * b[3]) / b[3]) * 0.5 * pr.size[1];
                const double cam_x = (was[0] / was[3] - now_x) * 0.5 * pr.size[0];
                const double cam_y = (now_y - was[1] / was[3]) * 0.5 * pr.size[1];
                const double apart = std::max(std::fabs(own_x - cam_x), std::fabs(own_y - cam_y));
                if (apart > most) most = apart, worst[0] = own_x, worst[1] = own_y, worst[2] = cam_x, worst[3] = cam_y;
                sum += apart, moved = std::max(moved, std::max(std::fabs(cam_x), std::fabs(cam_y))), n++;
            }
            if (n && moved > 2.0) {
                s.probes_told++;
                logf("vk: capture probe: frame %llu draw %016llx, %d vertices on screen: its own motion and the camera's are at "
                     "most %.3f px apart (%.3f on average; the camera's motion up to %.1f px); the worst: own %.2f %.2f, the "
                     "camera's %.2f %.2f", (unsigned long long)pr.frame, (unsigned long long)pr.key, n, most, sum / n, moved,
                     worst[0], worst[1], worst[2], worst[3]);
            }
            pr.used = false;
        }
        if (!pr.buffer && !create_buffer_cached(kProbeSlots * 32, VK_BUFFER_USAGE_TRANSFER_DST_BIT, pr.buffer, pr.memory, &pr.map))
            pr = {};
        // (the draw with the most indices: a large piece of the world)
        const Redraw* big = &draws[0];
        for (const Redraw& d : draws)
            if (d.c->k.count > big->c->k.count) big = &d;
        const auto holder = [&](uint64_t address, uint64_t bytes) -> const State::CaptureBuffer* {
            for (const auto& b : s.capture_buffers)
                if (b.buffer && address >= b.address && address + bytes <= b.address + b.size) return &b;
            return nullptr;
        };
        // (the same vertices in both: the slots both frames have, from the later first)
        const uint32_t first = std::max(big->push.first_now, big->push.first_before);
        const uint32_t skip_now = first - big->push.first_now, skip_before = first - big->push.first_before;
        const uint32_t count = skip_now < big->push.count_now && skip_before < big->push.count_before
                                   ? std::min({big->push.count_now - skip_now, big->push.count_before - skip_before, kProbeSlots}) : 0;
        const uint64_t from_now = big->push.now + (uint64_t)skip_now * 16, from_before = big->push.before + (uint64_t)skip_before * 16;
        const State::CaptureBuffer* bn = holder(from_now, (uint64_t)count * 16);
        const State::CaptureBuffer* bb = holder(from_before, (uint64_t)count * 16);
        if (pr.buffer && count && bn && bb) {
            const VkBufferCopy c0{from_now - bn->address, 0, (uint64_t)count * 16};
            const VkBufferCopy c1{from_before - bb->address, kProbeSlots * 16, (uint64_t)count * 16};
            vkCmdCopyBuffer(cmd, bn->buffer, pr.buffer, 1, &c0);
            vkCmdCopyBuffer(cmd, bb->buffer, pr.buffer, 1, &c1);
            pr.frame = now, pr.key = big->c->key, pr.count = count;
            std::memcpy(pr.reproject, reproject, sizeof pr.reproject);
            for (int i = 0; i < 2; i++) pr.jitter_now[i] = big->push.jitter_now[i], pr.jitter_before[i] = big->push.jitter_before[i];
            pr.size[0] = (float)rw, pr.size[1] = (float)rh;
            pr.used = true;
            full_barrier(cmd);
        }
    }
    // (the positions the scene's draws stored: the prep pass's barrier is after them)
    if (!begin_redraw(cmd, s.capture, cam, before, s.last_jitter, reproject, rw, rh, dview)) return 0;
    for (const Redraw& d : draws) {   // (the same indices, base vertex and topology: VertexIndex names the same vertex)
        const CaptureDraw& k = d.c->k;
        vkCmdPushConstants(cmd, s.actor_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof d.push, &d.push);
        vkCmdSetPrimitiveTopology(cmd, k.topology);
        vkCmdSetPrimitiveRestartEnable(cmd, k.restart ? VK_TRUE : VK_FALSE);
        if (k.itype != dlss_actor::kNoIndices) {
            const VkIndexType type = k.itype == 0 ? VK_INDEX_TYPE_UINT8_EXT : k.itype == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
            vkCmdBindIndexBuffer(cmd, d.ib.buffer, d.ib.offset, type);
            vkCmdDrawIndexed(cmd, (uint32_t)k.count, 1, 0, k.base_vertex, 0);
        } else {
            vkCmdDraw(cmd, (uint32_t)k.count, 1, (uint32_t)k.base_vertex, 0);
        }
    }
    vkCmdEndRendering(cmd);
    s.actor_draws += draws.size(), s.actor_frames++, s.capture_draws += caps.size();
    return (int)draws.size();
}

/// The bones a skinned draw's vertices use (the largest joint with a weight, plus one), read once a model; 0 when its
/// buffers cannot be read
uint32_t bones_used(const SkinnedDraw& k) {
    if (k.count <= 0 || k.count > (1 << 20) || k.itype < 0 || k.itype > dlss_actor::kNoIndices || !k.stride) return 0;
    uint32_t lo = UINT32_MAX, hi = 0;
    if (k.itype == dlss_actor::kNoIndices) {   // (a plain vertex array: base_vertex to base_vertex + count - 1)
        lo = 0, hi = (uint32_t)k.count - 1;
    } else {
        std::vector<uint8_t> idx((size_t)k.count << k.itype);
        if (!memory_read_as_submitted(k.ibuf, idx.data(), idx.size())) return 0;
        for (int i = 0; i < k.count; i++) {
            uint32_t v = 0;
            std::memcpy(&v, idx.data() + ((size_t)i << k.itype), (size_t)1 << k.itype);
            lo = std::min(lo, v), hi = std::max(hi, v);
        }
    }
    const int64_t first = (int64_t)k.base_vertex + lo, n = (int64_t)hi - lo + 1;
    if (first < 0 || n > (1 << 20) || (uint64_t)(first + n) * k.stride > k.vsize) return 0;
    std::vector<uint8_t> v((size_t)n * k.stride);
    if (!memory_read_as_submitted(k.vbuf + (uint64_t)first * k.stride, v.data(), v.size())) return 0;
    uint32_t most = 0;
    for (int64_t i = 0; i < n; i++) {
        const uint8_t* q = v.data() + (size_t)i * k.stride;
        for (int set = 0; set < k.sets; set++)
            for (int j = 0; j < dlss_actor::set_count(k.formats, set); j++) {   // (1, 2 or 4; rigid: one, no weight)
                if (!(k.formats & dlss_actor::kRigid) && !q[k.weights[set] + j]) continue;
                uint32_t joint = 0;
                if (k.formats & (dlss_actor::kByteJoints << set)) joint = q[k.joints[set] + j];
                else std::memcpy(&joint, q + k.joints[set] + 2 * j, 2);
                most = std::max(most, joint + 1);
            }
    }
    return most;
}

/// The chain's images and buffers let go (the engine's own: its release)
void drop_all() {
    State& s = st();
    for (Image* im : {&s.color, &s.depth, &s.motion, &s.output, &s.base, &s.merged}) drop_image(*im);
    for (auto& pb : s.pose_buffers)   // (the actor motion's poses and cameras)
        if (pb.buffer) {
            const State::PoseBuffer gone = pb;
            later([gone] { destroy_buffer(gone.buffer, gone.memory); });
            pb = {};
        }
    for (auto& u : s.uploads)
        if (u.buffer) {
            const State::Upload gone = u;
            later([gone] { destroy_buffer(gone.buffer, gone.memory); });
            u = {};
        }
    for (auto& pr : s.probes)
        if (pr.buffer) {
            const State::Probe gone = pr;
            later([gone] { destroy_buffer(gone.buffer, gone.memory); });
            pr = {};
        }
    for (auto& b : s.capture_buffers)   // (the stored positions)
        if (b.buffer) {
            const State::CaptureBuffer gone = b;
            later([gone] { destroy_buffer(gone.buffer, gone.memory); });
            b = {};
        }
    s.poses.clear(), s.skins.clear(), s.model_bones.clear();
    s.captured.clear(), s.captured_models.clear(), s.captured_before.clear(), s.index_ranges.clear();
    s.capture_used = 0, s.captured_frame = 0;
    if (!s.depth_views.empty()) {
        auto old = std::move(s.depth_views);
        s.depth_views.clear();
        later([old] {
            for (const auto& [image, view] : old) vkDestroyImageView(dev.device, view, nullptr);
        });
    }
}

}  // namespace

void temporal_later(std::function<void()> f) { later(std::move(f)); }

// (local, RenoDX port) The scene's formats: the game's B10G11R11, and RGBA16F in a run where the host chose it for the scene's
// colour target (nx2/oot_patches.py patch_scene_format; host/nn_vi.c hos_scene_rgba16f, by the same rule: HOS_SCENE_RGBA16F,
// else HDR Native at the start with RenoDX's shaders in the build and not RX_VK_RENODX=0). Not the menu's HDR row as it is
// now, which can change in play while the target cannot. The scene's target can be RGBA16F while the image the upscale
// network reads, which the game's tone mapping writes, stays B10G11R11: either is the scene's here
bool temporal_scene_format(VkFormat f) {
    static const bool rgba16f_run = [] {
        if (const char* e = getenv("HOS_SCENE_RGBA16F")) return atoi(e) != 0;
        return renodx_built() && hdr_at_start == 2 && renodx_chosen();
    }();
    return f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || (f == VK_FORMAT_R16G16B16A16_SFLOAT && rgba16f_run);
}

void temporal_note_scene_draw(const Texture* color0, uint32_t width, uint32_t height, Texture* depth, uint64_t camera,
                              uint64_t camera_size) {
    // (only a size the gameplay upscale's output can come from: 1 to 3 times smaller. A smaller target with a camera (the
    // pause screen's model views, a reflection) is never the scene; one this size that is not either is left by the checks
    // below: the format, the camera's own size and perspective, the largest in the frame, and the upscale's input)
    if (!color0 || !depth || !camera || camera_size < 0x230 || !temporal_scene_size(width, height)) return;
    // (AMD FSR 3.1 before the bloom keeps the camera here too: the point is found from the scene's target)
    if (!temporal_scene_format(color0->format) || !(temporal_wanted() || pre_wanted(ur::GAME))) return;
    State& s = st();
    std::lock_guard lk(s.mu);
    const uint64_t now = texture_frame_no();
    if (s.cam.valid && s.cam.frame == now && (uint64_t)width * height <= (uint64_t)s.cam.width * s.cam.height)
        return;   // (the largest one of the frame wins)
    float c[0x230 / 4];
    if (!memory_read_as_submitted(camera, c, sizeof c)) return;
    // (the target's size at 0x120; the projection at 0x70, its w row (0, 0, -1, 0) at 0xa0)
    if (c[0x120 / 4] != (float)width || c[0x124 / 4] != (float)height || c[0xa0 / 4] != 0.0f || c[0xa4 / 4] != 0.0f ||
        c[0xa8 / 4] != -1.0f || c[0xac / 4] != 0.0f)
        return;
    Camera& k = s.cam;
    std::memcpy(k.current, c + 0x30 / 4, sizeof k.current);
    std::memcpy(k.view, c, sizeof k.view), std::memcpy(k.proj, c + 0x70 / 4, sizeof k.proj);
    // (the offset sead's projection gave the frustum, in its third column: 2 * offset, which moves the picture by -that)
    k.jitter[0] = -c[0x78 / 4], k.jitter[1] = -c[0x88 / 4];
    // (its planes and field of view, for an engine that wants them (FSR's depth heuristics): an OpenGL-style projection, depth
    // -1 to 1, P[2][2] = -(f + n) / (f - n) and P[2][3] = -2fn / (f - n); 0 when they do not read as one)
    const float p11 = c[0x84 / 4], p22 = c[0x98 / 4], p23 = c[0x9c / 4];
    const float n = p23 / (p22 - 1.0f), f = p23 / (p22 + 1.0f);
    const bool plain = std::isfinite(n) && n > 0.0f && p11 > 0.0f && (!std::isfinite(f) || f > n);
    k.near_z = plain ? n : 0.0f, k.far_z = plain ? (std::isfinite(f) ? f : 1e9f) : 0.0f;
    k.fov_y = plain ? 2.0f * std::atan(1.0f / p11) : 0.0f;
    k.width = width, k.height = height, k.format = color0->format, k.color = color0, k.depth = depth, k.depth_gen = texture_destroy_generation(), k.frame = now;
    k.valid = true;
}

bool temporal_scene_size(uint32_t width, uint32_t height) {
    const State& s = st();
    return dlss_checks::scene_size_fits(width, height, s.out_w.load(std::memory_order_relaxed),
                                        s.out_h.load(std::memory_order_relaxed));
}

uint32_t temporal_jitter_size(int axis) {
    // (RX_VK_DLSS_JITTER=0: none. 0 too once no engine has drawn for a while: the camera as the game has it)
    static const bool off = env_switch("RX_VK_DLSS_JITTER") == 0;
    State& s = st();
    const int mode = temporal_mode();
    if (off || mode == ur::GAME) return 0;
    const uint64_t last = s.last_frame.load(std::memory_order_relaxed);
    if (s.engines[ur::is_fsr4(mode) ? 1 : 0].failed.load(std::memory_order_relaxed) || !last || texture_frame_no() - last > 30)
        return 0;
    if (axis == 2) return s.jitter_phases.load(std::memory_order_relaxed);
    return axis ? s.jitter_h.load(std::memory_order_relaxed) : s.jitter_w.load(std::memory_order_relaxed);
}

int temporal_status(char* b, size_t n) {
    State& s = st();
    const uint64_t last = s.last_frame.load(std::memory_order_relaxed);
    const int mode = temporal_mode();
    if (mode == ur::GAME || !last || texture_frame_no() - last > 30) return 0;
    std::lock_guard lk(s.mu);
    const Engine& k = engine_of(mode);
    if (k.failed || !k.e) return 0;
    // (the chain's own video memory: the images and buffers it made here, and what the engine says it holds, when it says)
    uint64_t own = 0;
    for (const Image* im : {&s.color, &s.depth, &s.motion, &s.output, &s.base, &s.merged})
        if (im->image) own += allocation_size(im->memory);
    for (const auto& pb : s.pose_buffers)
        if (pb.buffer) own += allocation_size(pb.memory);
    for (const auto& u : s.uploads)
        if (u.buffer) own += allocation_size(u.memory);
    for (const auto& b : s.capture_buffers)
        if (b.buffer) own += allocation_size(b.memory);
    const uint64_t engine = k.e->memory();
    const uint64_t draws = s.actor_draws - s.status_draws, frames = s.actor_frames - s.status_frames;
    char feature[48] = "";
    if (engine) snprintf(feature, sizeof feature, ", its own %llu MB", (unsigned long long)(engine >> 20));
    char passed[64] = "";
    if (s.status_passed) snprintf(passed, sizeof passed, ", %llu left to the game's network", (unsigned long long)s.status_passed);
    // (capture motion: the draws that stored positions, a frame, and the most a frame's took)
    char stored[96] = "";
    if (motion_mode() == kMotionCapture && frames)
        snprintf(stored, sizeof stored, " of %llu stored a frame (%llu vertices, %.1f MB at most; %llu with another jitter)",
                 (unsigned long long)(s.capture_draws / frames), (unsigned long long)(s.capture_slots / frames),
                 s.capture_bytes_peak / 1048576.0, (unsigned long long)(s.capture_unjittered / frames));
    snprintf(b, n, "; %s: %llu frames drawn%s, %llu made, jitter %+.3f %+.3f px%s, %s motion %llu draws%s in %llu "
             "frames (%.3f ms CPU a frame), its images %llu MB%s",
             name_of(mode), (unsigned long long)s.status_drawn, passed, (unsigned long long)s.engines_made,
             s.status_jitter[0], s.status_jitter[1], s.status_motion ? "" : " (no motion)",
             motion_mode() == kMotionCapture ? "captured" : "actor", (unsigned long long)draws, stored,
             (unsigned long long)frames, frames ? s.actor_ns / 1e6 / (double)frames : 0.0, (unsigned long long)(own >> 20), feature);
    s.status_draws = s.actor_draws, s.status_frames = s.actor_frames, s.actor_ns = 0, s.status_drawn = s.status_passed = 0;
    s.capture_draws = s.capture_slots = s.capture_bytes_peak = s.capture_unjittered = 0;
    return mode;
}

void temporal_idle() {
    State& s = st();
    if (!s.holding.load(std::memory_order_relaxed)) return;
    std::lock_guard lk(s.mu);
    const uint64_t now = texture_frame_no();
    if (now - s.asked <= 120) return;   // (in use, or off for under 120 frames (2 s): kept, a row stepped through comes back at once)
    // (hookshot, a stutter: kept too while DLSS or FSR 4 is still the Upscaler's choice. Each area change's loading screen has no
    // gameplay upscale for 2 s and more, and DLSS's feature let go then was made again at the next area's first frame: 44-78 ms
    // of the replay thread at every area entry (12 times in a 15-minute walk, 2026-11-10, #1520). Let go once the row leaves them.
    // RX_VK_DLSS_IDLE_RELEASE=1: let go after 120 frames unused, as before)
    static const bool release_idle = getenv("RX_VK_DLSS_IDLE_RELEASE") && atoi(getenv("RX_VK_DLSS_IDLE_RELEASE")) != 0;
    if (!release_idle && temporal_wanted()) return;
    if (s.drawing) {
        logf("vk: %s: not used for 120 frames (the last at frame %llu, now %llu): what it made and its images let go; "
             "the next upscale with it makes them again", s.drawing->name, (unsigned long long)s.asked, (unsigned long long)now);
        s.drawing->release();
        s.drawing = nullptr;
    }
    drop_all();
    s.last_frame = 0;   // (a new history when it comes back)
    free_dead(false);
    s.holding = !s.dead.empty();
}

const char* temporal_note_skinned_draw(const SkinnedDraw& k) {
    if (motion_mode() == kMotionCapture) return "its stored positions are drawn instead";
    if (motion_mode() != kMotionBones || !temporal_wanted()) return "actor motion off";
    if (!k.depth || !k.bones || k.bones_size < 48 || !k.object || k.object_size < 0x4c) return "no object buffer";
    State& s = st();
    std::lock_guard lk(s.mu);
    if (s.skins.size() >= 512) s.skins.clear();   // (no upscale for a while: what was kept is stale)
    struct Timed {
        State& s;
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        ~Timed() { s.actor_ns += (std::chrono::steady_clock::now() - t0).count(); }
    } timed{s};
    State::Skin d;
    d.k = k;
    const uint64_t model = dlss_actor::key(k.vbuf, k.ibuf, k.count, k.base_vertex, k.program, 0);
    // (only the bones its vertices use are taken: a palette holds up to 280, a model uses a few dozen)
    auto mb = s.model_bones.find(model);
    if (mb == s.model_bones.end()) {
        if (s.model_bones.size() > 4096) s.model_bones.clear();
        mb = s.model_bones.emplace(model, bones_used(k)).first;
    }
    d.nbones = (uint32_t)std::min<uint64_t>(mb->second, std::min<uint64_t>(k.bones_size / 48, 280));
    if (!d.nbones) return "its vertices use no bone (or its buffers could not be read)";
    // (its pose now, as the game submitted the draw: by the upscale the game may have written the next frame's there)
    // (and the camera it was drawn with: its view and projection, which the redraw uses for this frame's position)
    d.pose.assign(dlss_actor::kPoseHead + (size_t)d.nbones * 48, 0);
    if (!memory_read_as_submitted(k.object + 0x40, d.pose.data(), 12) ||
        !memory_read_as_submitted(k.bones, d.pose.data() + dlss_actor::kPoseHead, (uint64_t)d.nbones * 48))
        d.pose.clear();   // (copied on the GPU at the upscale, as before)
    else if (!k.camera || k.camera_size < 0xb0 || !memory_read_as_submitted(k.camera, d.pose.data() + 16, 48) ||
             !memory_read_as_submitted(k.camera + 0x70, d.pose.data() + 64, 64))
        d.pose.clear();
    int nth = 0;   // (the same model drawn twice in a frame: the n-th draw of it is the n-th the frame before)
    for (const auto& o : s.skins) nth += dlss_actor::same_model(o.key, model);
    d.key = dlss_actor::key(k.vbuf, k.ibuf, k.count, k.base_vertex, k.program, nth);
    s.skins.push_back(std::move(d));
    return nullptr;
}

bool temporal_capture_wanted() { return motion_mode() == kMotionCapture && temporal_wanted(); }

void temporal_capture_draw(const CaptureDraw& k, CaptureSlot* out) {
    *out = {};
    if (!k.depth || k.count <= 0 || !temporal_capture_wanted()) return;
    State& s = st();
    std::lock_guard lk(s.mu);
    struct Timed {
        State& s;
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        ~Timed() { s.actor_ns += (std::chrono::steady_clock::now() - t0).count(); }
    } timed{s};
    if (s.capture_ok < 0) {   // (the stores are by device address)
        VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        vkGetPhysicalDeviceFeatures2(dev.physical, &f);
        // (and stores from a vertex stage: every desktop GPU has them, and the device is made with all it has)
        s.capture_ok = dev.vk12.bufferDeviceAddress && f.features.shaderInt64 && dev.features.vertexPipelineStoresAndAtomics;
        if (!s.capture_ok) logf("vk: upscaler: no stores from vertex shaders by address on this GPU: the camera's motion alone");
    }
    if (!s.capture_ok) return;
    // (a frame went by without an upscale taking them (a loading screen, a menu over the scene): what was kept is stale, its
    // buffer given out again. Left to pile up, the buffer ran out there, and was made twice as large for nothing)
    const uint64_t frame = texture_frame_no();
    if (frame > s.capture_frame + 1 || s.captured.size() >= 32768) {
        s.captured.clear(), s.captured_models.clear();
        s.capture_used = 0, s.capture_full = false;
    }
    if (s.captured.empty()) s.capture_frame = frame;
    // (an indexed draw's vertices: from its least index to its greatest, read once for its index buffer)
    uint32_t lo = 0, hi = 0;
    if (k.itype != dlss_actor::kNoIndices) {
        if (k.itype < 0 || k.itype > 2 || k.count > (1 << 24)) return;
        const uint64_t at = dlss_actor::key(k.ibuf, (uint64_t)k.itype, k.count, k.restart ? 1 : 0, 0, 0);
        auto it = s.index_ranges.find(at);
        if (it == s.index_ranges.end()) {
            if (s.index_ranges.size() > 65536) s.index_ranges.clear();
            State::IndexRange r;
            std::vector<uint8_t> idx((size_t)k.count << k.itype);
            if (memory_read_as_submitted(k.ibuf, idx.data(), idx.size()))
                r.ok = dlss_actor::index_range(idx.data(), k.count, k.itype, k.restart, &r.lo, &r.hi);
            it = s.index_ranges.emplace(at, r).first;
        }
        if (!it->second.ok) return;
        lo = it->second.lo, hi = it->second.hi;
    }
    uint32_t first = 0, n = 0;
    if (!dlss_actor::capture_span(k.itype, k.count, k.base_vertex, lo, hi, kCaptureMostSlots, &first, &n)) return;
    // (this frame's buffer: RX_VK_DLSS_POSITIONS_MB, 32 MB (two million vertices) at first; more when a frame ran out)
    if (!s.capture_want) {
        const int mb = env_switch("RX_VK_DLSS_POSITIONS_MB");
        s.capture_want = std::min<uint64_t>((uint64_t)(mb > 0 ? mb : 32) << 20, kCaptureMost);
    }
    State::CaptureBuffer& b = s.capture_buffers[s.capture_at];
    if (!b.buffer) {
        if (!create_device_buffer(s.capture_want, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT,   // (RX_VK_DLSS_PROBE copies from it)
                                  b.buffer, b.memory)) {
            b = {};
            s.capture_full = true;
            return;
        }
        b.size = s.capture_want;
        b.address = device_address({b.buffer, 0, b.size, 0});
        if (!b.address) {
            destroy_buffer(b.buffer, b.memory);
            b = {};
            s.capture_ok = 0;
            return;
        }
    }
    const uint64_t bytes = (uint64_t)n * 16;
    if (s.capture_used + bytes > b.size) {
        s.capture_full = true;
        return;
    }
    // (its name across frames: as the actor motion's, the n-th draw of a model this frame the n-th the frame before)
    const uint64_t model = dlss_actor::key(k.vbuf, k.ibuf, k.count, k.base_vertex, k.program, 0);
    const int nth = s.captured_models[model >> 8]++;
    State::Captured c;
    c.k = k, c.key = dlss_actor::key(k.vbuf, k.ibuf, k.count, k.base_vertex, k.program, nth);
    c.address = b.address + s.capture_used, c.first = first, c.count = n;
    // (the jitter of the camera it is drawn with, as submitted: read again when the address or the captures' view changes)
    if (k.camera && k.camera_size >= 0xb0) {
        const uint64_t view = memory_capture_view();
        if (k.camera != s.capture_camera || view != s.capture_camera_view) {
            float proj[16];
            s.capture_camera = k.camera, s.capture_camera_view = view;
            s.capture_camera_own = memory_read_as_submitted(k.camera + 0x70, proj, sizeof proj) &&
                                   dlss_actor::camera_jitter(proj, s.capture_camera_jitter);
        }
        c.own = s.capture_camera_own;
        c.jitter[0] = s.capture_camera_jitter[0], c.jitter[1] = s.capture_camera_jitter[1];
    }
    s.captured.push_back(c);
    s.capture_used += bytes, s.capture_slots += n;
    out->address = c.address, out->first = first, out->count = n;
}

namespace {
bool chain(VkCommandBuffer cmd, int mode, Engine& engine, const Camera& cam, const std::vector<State::Captured>& captures,
           VkImageView in_view, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, VkImageView out_view, const Texture* pre,
           VkFormat in_format, VkFormat out_format);

/// The engine before the game's bloom and tone mapping (temporal_pre_point, temporal_pre_tonemap): NVIDIA DLSS and DLAA,
/// AMD FSR 4 and its native anti-aliasing (both engines take linear HDR colour), and AMD FSR 3.1 (vk_fsr3.cpp: its own
/// inputs and passes, the rest of this the same; mode is then the game's own, as for the chain). RX_VK_DLSS_PRE=0: as before, in place of
/// the upscale's dispatch, on the tone mapped picture (the name is DLSS's from before FSR came: it applies to either)
bool pre_wanted(int mode) {
    // (Triforce, 11-13: off until the Upscale before bloom row turns it on, read at the start; RX_VK_DLSS_PRE=0 or 1 over it)
    static const bool on = [] {
        const int e = env_switch("RX_VK_DLSS_PRE");
        return e >= 0 ? e != 0 : upscale_pre_at_start != 0;
    }();
    return on && (ur::is_dlss(mode) || ur::is_fsr4(mode) || fsr3_pre_wanted());
}

/// The upscale's dispatch in a frame the engine drew before the bloom: the tone mapped picture into its output, nothing
/// else. From final, the tone mapping draw's own target (linear, before the game's edge anti-aliasing, which the engine
/// has done better); without it from the dispatch's input (that picture's square root after the game's anti-aliasing, as
/// the network samples it), squared. false: the game's own pass runs
bool pre_copy(VkCommandBuffer cmd, Texture* final, Texture* in, VkImageView in_view, Texture* out, VkImageView out_view,
              uint32_t out_level) {
    State& s = st();
    if (final) {
        if (const VkImageView v = depth_view(final)) in = final, in_view = v;
        else final = nullptr;
    }
    if (!in || !in_view || !out || !out_view || !temporal_scene_format(out->format)) return false;
    const bool wide = out->format == VK_FORMAT_R16G16B16A16_SFLOAT;   // (an RGBA16F picture: the pass's -DHDR layout)
    if (!make_pipelines(wide)) return false;
    const uint32_t ow = std::max(1u, out->width >> out_level), oh = std::max(1u, out->height >> out_level);
    struct Disturbed {
        ~Disturbed() { descriptors_disturbed(); }
    } disturbed;
    const int32_t opc[3] = {(int32_t)ow, (int32_t)oh, final ? 1 : 2};
    const VkDescriptorImageInfo info[2] = {
        {in->width == ow && in->height == oh ? s.point : s.linear, in_view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, out_view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet w[2]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    w[0].dstBinding = 0, w[0].descriptorCount = 1, w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[0].pImageInfo = &info[0];
    w[1].dstBinding = 1, w[1].descriptorCount = 1, w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[1].pImageInfo = &info[1];
    full_barrier(cmd);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, wide ? s.out_hdr : s.out);
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.out_layout, 0, 2, w);
    vkCmdPushConstants(cmd, s.out_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof opc, opc);
    vkCmdDispatch(cmd, (ow + 7) / 8, (oh + 7) / 8, 1);
    return true;
}
/// The upscale's dispatch, for the engine before the bloom (the chain's lock held): true when the output is written (the
/// tone mapped picture drawn there at its size, or copied there), and the dispatch is left out. In a frame the engine
/// did not draw before the bloom, the point is learnt for the next
bool pre_dispatch(VkCommandBuffer cmd, int mode, Texture* in, VkImageView in_view, Texture* out, VkImageView out_view,
                  uint32_t out_level, bool learn = true) {
    State& s = st();
    // (the engine drew before the bloom, and this is the scene's upscale: the tone mapped picture goes to the output as it
    // is, or is there already, drawn at the output's size)
    // (its input is the scene's own texture with the game's formats, the game packing the tone mapped picture back into it;
    // with an RGBA16F picture (local, RenoDX port) it is another texture of the scene's size: known by its size and format)
    if (s.pre_pending && in && s.pre_scene && in->width == s.pre_scene->width && in->height == s.pre_scene->height &&
        temporal_scene_format(in->format)) {
        const bool direct = s.pre_direct && out && out == s.pre_out;
        Texture* final = const_cast<Texture*>(s.pre_final);
        s.pre_pending = false, s.pre_candidate = {}, s.pre_final = nullptr, s.pre_direct = false;
        if (out && !out_level) s.pre_out = out, s.pre_out_gen = texture_destroy_generation();
        if (out && temporal_scene_format(out->format)) note_output(out, out_level);
        return direct || pre_copy(cmd, final, in, in_view, out, out_view, out_level);
    }
    if (learn && pre_wanted(mode)) {   // (it did not: what the first draw after the scene was, for the next frame)
        if (s.pre_candidate.program && !(s.pre_candidate == s.pre_key)) {
            logf("vk: %s: before the game's bloom and tone mapping from the next frame: the first draw after the scene is "
                 "program %016llx into %ux%u, format %d (frame %llu)", ur::is_dlss(mode) || ur::is_fsr4(mode) ? name_of(mode) : "AMD FSR 3.1",
                 (unsigned long long)s.pre_candidate.program, s.pre_candidate.width, s.pre_candidate.height,
                 (int)s.pre_candidate.format, (unsigned long long)texture_frame_no());
            s.pre_key = s.pre_candidate;
        }
        s.pre_candidate = {}, s.pre_missed++;
        // (a frame or two without the point, between frames that had it: the tone mapped picture as it is, in place of the
        // engine made again for LDR colour and then again for HDR, a stall each)
        if (s.pre_drawn && ++s.pre_miss_run <= 2 && in && s.cam.valid && in->width == s.cam.width) {
            s.cam.valid = false, s.skins.clear();
            (void)take_captures();
            if (out && temporal_scene_format(out->format)) note_output(out, out_level);
            if (pre_copy(cmd, nullptr, in, in_view, out, out_view, out_level)) return true;
        }
    }
    return false;
}

/// AMD FSR 3.1 before the bloom: its picture (vk_fsr3.cpp fsr3_pre) back into the scene's colour target at the scene's
/// size, and the same into the copy when it scaled up, as the chain's own pass does for its engines
bool pre_fsr3(VkCommandBuffer cmd, Texture* scene, VkImageView view, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh) {
    State& s = st();
    const bool scaled = ow != rw || oh != rh;
    if (!make_pipelines(false) || (scaled && !make_image(cmd, s.base, VK_FORMAT_B10G11R11_UFLOAT_PACK32, rw, rh))) return false;
    struct Disturbed {
        ~Disturbed() { descriptors_disturbed(); }
    } disturbed;
    s.cam.valid = false, s.skins.clear();   // (each frame's own: FSR 3.1 keeps a camera of its own)
    (void)take_captures();
    s.asked = texture_frame_no(), s.holding = true;
    VkImageView picture = VK_NULL_HANDLE;
    if (!fsr3_pre(cmd, scene, view, ow, oh, &picture) || !picture) return false;
    full_barrier(cmd);
    const int32_t opc[3] = {(int32_t)rw, (int32_t)rh, 1};
    VkDescriptorImageInfo info[2] = {{scaled ? s.linear : s.point, picture, VK_IMAGE_LAYOUT_GENERAL},
                                     {VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet w[2]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    w[0].dstBinding = 0, w[0].descriptorCount = 1, w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[0].pImageInfo = &info[0];
    w[1].dstBinding = 1, w[1].descriptorCount = 1, w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[1].pImageInfo = &info[1];
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.out);
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.out_layout, 0, 2, w);
    vkCmdPushConstants(cmd, s.out_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof opc, opc);
    vkCmdDispatch(cmd, (rw + 7) / 8, (rh + 7) / 8, 1);
    if (scaled) {
        info[1].imageView = s.base.view;
        vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.out_layout, 0, 2, w);
        vkCmdDispatch(cmd, (rw + 7) / 8, (rh + 7) / 8, 1);
    }
    full_barrier(cmd);
    s.pre_pending = true, s.pre_frame = texture_frame_no(), s.pre_scene = scene, s.pre_drawn++;
    s.pre_final = nullptr, s.pre_direct = false, s.pre_miss_run = 0, s.pre_touched = false;
    s.pre_picture = {picture, ow, oh};
    return true;
}
}  // namespace

bool temporal_upscale(VkCommandBuffer cmd, uint64_t key, bool known, Texture* in, VkImageView in_view, Texture* out,
                      VkImageView out_view, uint32_t out_level) {
    const int mode = temporal_mode();
    if (mode == ur::GAME) return false;
    State& s = st();
    std::lock_guard lk(s.mu);
    if (pre_dispatch(cmd, mode, in, in_view, out, out_view, out_level)) return true;
    if (!known) {   // (another network of the upscale's shape: only the scene into the picture, the rest untouched and unsaid)
        const Camera& c = s.cam;
        // (the gameplay networks' own formats: they sample the scene's B10G11R11 colour and write the B10G11R11 picture.
        // Other cooperative-matrix networks of the same shape (one RGBA8 into A2B10G10R10, RGB9E5 into B10G11R11) are not it)
        if (!in || !out || !temporal_scene_format(in->format) || out->format != in->format ||
            (out->width >> out_level) <= in->width)
            return false;
        note_output(out, out_level);
        // (one in the gameplay networks' formats whose input is not this frame's scene: counted for the status line)
        if (!c.valid || texture_frame_no() - c.frame > 1 || in->width != c.width || in->height != c.height)
            return s.status_passed++, false;
        static std::atomic<uint64_t> told{0};
        if (told.exchange(key) != key) logf("vk: upscaler: network %016llx is the gameplay upscale (%ux%u -> %ux%u)",
                                            (unsigned long long)key, in->width, in->height, out->width >> out_level,
                                            out->height >> out_level);
    }
    free_dead(false);
    if (known && out && temporal_scene_format(out->format)) note_output(out, out_level);
    const Camera cam = s.cam;
    s.cam.valid = false;   // (each frame's own camera)
    struct SkinsDone {     // (the skinned draws, each frame's own too: actor_motion takes them, or they go here)
        State& s;
        ~SkinsDone() { s.skins.clear(); }
    } skins_done{s};
    // (and the draws that stored their positions: the next frame's go into the next buffer of the ring)
    const std::vector<State::Captured> captures = take_captures();

    Engine& engine = engine_of(mode);
    if (!engine.e || engine.failed) return fail("it failed earlier");
    s.asked = texture_frame_no(), s.holding = true;   // (not after a failure: what it holds is let go, temporal_idle)
    if (!in || !in_view || !out || !out_view) return fail("no input or output view");
    if (!cam.valid || texture_frame_no() - cam.frame > 1) return fail("no scene camera this frame");
    if (cam.depth_gen != texture_destroy_generation()) return fail("the scene depth may be gone");
    const uint32_t rw = in->width, rh = in->height;
    const uint32_t ow = std::max(1u, out->width >> out_level), oh = std::max(1u, out->height >> out_level);
    if (rw != cam.width || rh != cam.height) return fail("the input is not the scene's size");
    if (cam.depth->width != rw || cam.depth->height != rh || !(cam.depth->usage & VK_IMAGE_USAGE_SAMPLED_BIT) ||
        cam.depth->samples != VK_SAMPLE_COUNT_1_BIT || !(cam.depth->aspect & VK_IMAGE_ASPECT_DEPTH_BIT))
        return fail("unexpected scene depth");
    if (!temporal_scene_format(out->format) || out->format != in->format || !dlss_checks::scene_size_fits(rw, rh, ow, oh))
        return fail("unexpected output");
    return chain(cmd, mode, engine, cam, captures, in_view, rw, rh, ow, oh, out_view, nullptr, in->format, out->format);
}

namespace {
/// The engine's frame: its inputs made from in_view (the scene's colour as it is sampled) and the camera's depth and motion,
/// its evaluation, and its picture written: into out_view (the upscale's output, squared back to linear), or with pre (the
/// scene's colour target, before the game's bloom and tone mapping: in_view is linear HDR light) back into pre itself, of
/// which out_view is then a view. Called with the chain's lock held
bool chain(VkCommandBuffer cmd, int mode, Engine& engine, const Camera& cam, const std::vector<State::Captured>& captures,
           VkImageView in_view, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, VkImageView out_view, const Texture* pre,
           VkFormat in_format, VkFormat out_format) {
    State& s = st();
    // (the engine is given linear HDR colour: before the bloom, or an RGBA16F scene's picture (local, RenoDX port), which
    // only DLSS took; else the network's input, tonemapped, LDR. wide: the image the picture is written into is RGBA16F,
    // the passes' -DHDR layout)
    const bool hdr = pre != nullptr || in_format == VK_FORMAT_R16G16B16A16_SFLOAT;
    const bool wide = out_format == VK_FORMAT_R16G16B16A16_SFLOAT;
    if (hdr && !pre && !ur::is_dlss(mode)) return fail("an HDR scene: only DLSS takes it");
    if (!engine.started) engine.started = true, engine.ok = engine.e->start();
    if (!engine.ok) {
        engine.failed = true;
        return fail("not available");
    }
    if (!make_pipelines(wide || (hdr && !pre))) {
        engine.failed = true;
        return fail("its shaders were not made");
    }
    const VkImageView dview = depth_view(cam.depth);
    if (!dview) return fail("no view of the scene depth");
    if (s.drawing != engine.e) {   // (another engine than the one before: that one's state let go, and a new history)
        if (s.drawing) {
            logf("vk: upscaler: %s in place of %s (frame %llu)", engine.e->name, s.drawing->name,
                 (unsigned long long)texture_frame_no());
            s.drawing->release();
        }
        s.drawing = engine.e;
        s.last_frame = 0;
    }
    // (DLAA, FSR's native AA: the engine at the render size, anti-aliasing alone; its picture then scaled to the output's by
    // dlss_out.comp. With them the 3D is drawn at full scale, temporal_full_scale)
    const bool native = ur::full_scale(mode);
    const uint32_t dw = native ? rw : ow, dh = native ? rh : oh;
    // (from here commands are recorded, the engine's own too: whichever way this returns, the program's descriptor pushes are
    // gone from the bind point and are pushed again)
    struct Disturbed {
        ~Disturbed() { descriptors_disturbed(); }
    } disturbed;
    uint32_t iw = rw, ih = rh;
    const char* why = nullptr;
    const int ready = engine.e->prepare(cmd, rw, rh, dw, dh, hdr, &iw, &ih, &why);
    if (ready < 0) {
        engine.failed = true;
        return fail(why ? why : "it could not be made");
    }
    if (ready == 0) return fail(why ? why : "not this frame");
    if (ready == 2) s.engines_made++, s.last_frame = 0;   // (made just now: a new history)
    if (!make_image(cmd, s.color, VK_FORMAT_R16G16B16A16_SFLOAT, iw, ih) ||
        !make_image(cmd, s.depth, VK_FORMAT_R32_SFLOAT, iw, ih) ||
        !make_image(cmd, s.motion, VK_FORMAT_R16G16_SFLOAT, iw, ih, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ||
        !make_image(cmd, s.output, VK_FORMAT_R16G16B16A16_SFLOAT, dw, dh) ||
        (pre && !native && !make_image(cmd, s.base, pre->format, rw, rh)))
        return fail("its images were not made");

    const uint64_t now = texture_frame_no();
    const uint64_t last = s.last_frame;
    const bool reset = !last || now - last > 2;
    struct {
        float reproject[16];
        float jitter[2];
        int32_t size[2];
        int32_t flags;
    } pc{};
    // (RX_VK_DLSS_DEBUG=actors: the same view, the actor motion's pixels marked in it: actor_motion)
    static const bool show_motion = getenv("RX_VK_DLSS_DEBUG") && (!strcmp(getenv("RX_VK_DLSS_DEBUG"), "motion") ||
                                                                   !strcmp(getenv("RX_VK_DLSS_DEBUG"), "actors"));
    // (RX_VK_DLSS_DEBUG=characters: grey, and blue where a draw's own motion is not the camera's: redraw_tolerance)
    static const bool show_own = getenv("RX_VK_DLSS_DEBUG") && !strcmp(getenv("RX_VK_DLSS_DEBUG"), "characters");
    // (the motion without the jitter: from this frame's jittered clip space to the frame before's without its jitter.
    // A jitter moves clip x by jitter * w: taken off the previous matrix's first two rows)
    // (the frame before's camera as the engine last drew it: the buffer's own slot for it, 0x180, holds zeros in this version)
    float previous[16];
    std::memcpy(previous, s.last_vp, sizeof previous);
    dlss_math::unjitter(previous, s.last_jitter);
    const bool motion = !reset && dlss_math::reprojection(cam.current, previous, pc.reproject);
    pc.jitter[0] = cam.jitter[0], pc.jitter[1] = cam.jitter[1];
    pc.size[0] = (int32_t)rw, pc.size[1] = (int32_t)rh, pc.flags = (motion ? 1 : 0) | (show_motion ? 2 : 0) | (pre ? 16 : 0);

    const auto prep = [&] {
        const VkDescriptorImageInfo info[5] = {{s.point, in_view, VK_IMAGE_LAYOUT_GENERAL},
                                               {s.point, dview, VK_IMAGE_LAYOUT_GENERAL},
                                               {VK_NULL_HANDLE, s.color.view, VK_IMAGE_LAYOUT_GENERAL},
                                               {VK_NULL_HANDLE, s.depth.view, VK_IMAGE_LAYOUT_GENERAL},
                                               {VK_NULL_HANDLE, s.motion.view, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet w[5]{};
        for (uint32_t i = 0; i < 5; i++) {
            w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w[i].dstBinding = i, w[i].descriptorCount = 1, w[i].pImageInfo = &info[i];
            w[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hdr && !pre ? s.prep_hdr : s.prep);
        vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.prep_layout, 0, 5, w);
        vkCmdPushConstants(cmd, s.prep_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
        vkCmdDispatch(cmd, (rw + 7) / 8, (rh + 7) / 8, 1);
    };
    full_barrier(cmd);   // (the scene's colour and depth, written before)
    prep();
    full_barrier(cmd);
    // (what moves by itself: the draws drawn again over the camera's motion from the positions they stored, with their own;
    // or the skinned draws from their bones (RX_VK_DLSS_ACTORS=bones))
    const int actors = motion_mode() == kMotionCapture
                           ? capture_motion(cmd, cam, previous, pc.reproject, rw, rh, dview, motion, captures)
                           : actor_motion(cmd, cam, previous, pc.reproject, rw, rh, dview, motion);
    if (actors) full_barrier(cmd);
    if ((show_motion && actors) || show_own) {   // (RX_VK_DLSS_DEBUG=motion: the motion as it is now, the actors' in it)
        pc.flags = show_own ? 4 | 8 : 4;
        prep();
        full_barrier(cmd);
    }
    TemporalFrame f;
    f.color = s.color.shown(), f.depth = s.depth.shown(), f.motion = s.motion.shown(), f.output = s.output.shown();
    f.render_w = rw, f.render_h = rh;
    // (the jitter in render pixels, x right and y down)
    dlss_math::jitter_to_pixels(cam.jitter, rw, rh, f.jitter);
    f.reset = reset;
    if (cam.near_z > 0.0f) f.near_z = cam.near_z, f.far_z = cam.far_z, f.fov_y = cam.fov_y;
    if (!s.told_camera && cam.near_z > 0.0f) {
        s.told_camera = true;
        logf("vk: upscaler: the scene's camera: near %.3f, far %.1f, vertical field of view %.1f degrees", cam.near_z,
             cam.far_z, cam.fov_y * 57.29578f);
    }
    if (!engine.e->evaluate(cmd, f)) {
        logf("vk: %s: its evaluation failed: the game's upscale from now on", engine.e->name);
        engine.failed = true;
        return false;   // (the network writes its output over whatever the evaluation did)
    }
    full_barrier(cmd);
    {
        // (pre: the engine's picture back into the scene's colour target, at the scene's size: scaled down when the engine
        // scaled up, for the bloom and what else reads the scene. The picture itself is drawn from by the tone mapping,
        // temporal_pre_composite)
        if (pre) ow = rw, oh = rh;
        const int32_t opc[3] = {(int32_t)ow, (int32_t)oh, pre ? 1 : 0};
        const VkDescriptorImageInfo info[2] = {{native != (pre != nullptr) ? s.linear : s.point, s.output.view, VK_IMAGE_LAYOUT_GENERAL},
                                               {VK_NULL_HANDLE, out_view, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet w[2]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        w[0].dstBinding = 0, w[0].descriptorCount = 1, w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[0].pImageInfo = &info[0];
        w[1].dstBinding = 1, w[1].descriptorCount = 1, w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[1].pImageInfo = &info[1];
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, wide ? s.out_hdr : s.out);
        vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.out_layout, 0, 2, w);
        vkCmdPushConstants(cmd, s.out_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof opc, opc);
        vkCmdDispatch(cmd, (ow + 7) / 8, (oh + 7) / 8, 1);
        if (pre && !native) {   // (the same once more into the copy: what the scene holds now, texel for texel)
            const VkDescriptorImageInfo copy{VK_NULL_HANDLE, s.base.view, VK_IMAGE_LAYOUT_GENERAL};
            w[1].pImageInfo = &copy;
            vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.out_layout, 0, 2, w);
            vkCmdDispatch(cmd, (ow + 7) / 8, (oh + 7) / 8, 1);
        }
    }
    {   // (RX_VK_DLSS_BENCH=1: each mode's turn timed, from its 60th upscale to its last, and said when it ends)
        static const bool bench = env_switch("RX_VK_DLSS_BENCH") > 0;
        if (bench) {
            static std::chrono::steady_clock::time_point from;
            static uint64_t actor_draws_from = 0;
            const uint64_t n = bench_upscales.load(std::memory_order_relaxed), at = n % kBenchFrames;
            if (at == 60) from = std::chrono::steady_clock::now(), actor_draws_from = s.actor_draws;
            if (at == kBenchFrames - 1) {
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - from).count();
                const double frames = (double)(kBenchFrames - 1 - 60);
                static const char* const names[3] = {"the camera's motion alone", "the bones", "the stored positions"};
                logf("vk: upscaler bench: %s: %.3f ms a frame (%.1f fps) over %d frames at %ux%u, %.0f draws drawn again a frame",
                     names[(n / kBenchFrames) % 3], ms / frames, 1000.0 * frames / ms, (int)frames, rw, rh,
                     (double)(s.actor_draws - actor_draws_from) / frames);
            }
            bench_upscales.fetch_add(1, std::memory_order_relaxed);
        }
    }
    s.last_frame = now;
    s.last_jitter[0] = cam.jitter[0], s.last_jitter[1] = cam.jitter[1];
    // (for the status line, temporal_status: the jitter the engine was given, in render pixels)
    s.status_jitter[0] = f.jitter[0], s.status_jitter[1] = f.jitter[1], s.status_motion = motion;
    s.status_drawn++;
    if (pre) {
        s.pre_pending = true, s.pre_frame = now, s.pre_scene = pre, s.pre_drawn++;
        s.pre_final = nullptr, s.pre_direct = false, s.pre_miss_run = 0, s.pre_touched = false;
        s.pre_picture = {s.output.view, s.output.width, s.output.height};
        full_barrier(cmd);
    }

    std::memcpy(s.last_vp, cam.current, sizeof s.last_vp);
    s.jitter_w = rw, s.jitter_h = rh, s.jitter_phases = (uint32_t)dlss_jitter_phases(rw, rh, dw, dh);
    if (s.reason) logf("vk: %s draws the upscale again (frame %llu)", engine.e->name, (unsigned long long)now);
    s.reason = nullptr;
    // (RX_VK_DLSS_DEBUG=alternate, a test's reference: every other frame the game's own upscale over the engine's, which still
    // draws and keeps its history. A frame of it then has the game's picture of the frames either side to be measured against)
    static const bool alternate = getenv("RX_VK_DLSS_DEBUG") && !strcmp(getenv("RX_VK_DLSS_DEBUG"), "alternate");
    if (alternate && (now & 1) && !pre) return false;
    if (s.told_first != engine.e) {
        s.told_first = engine.e;
        // (the formats too: the scene's target can be RGBA16F while the network reads B10G11R11. hdr: the colour goes to the
        // engine as linear HDR)
        logf("vk: %s draws OoT's upscale: %ux%u -> %ux%u%s; the scene's target format %d, the network's input %d and output %d%s",
             name_of(mode), rw, rh, dw, dh, show_motion ? " (its motion shown)" : "", (int)cam.format, (int)in_format,
             (int)out_format, pre ? ": before the game's bloom and tone mapping, in the scene's colour target, HDR colour" : "");
    }
    return true;
}
}  // namespace

bool temporal_pre_point(const Texture* color0, uint64_t program, bool* late) {
    if (!color0 || !pre_wanted(temporal_mode())) return false;
    State& s = st();
    std::lock_guard lk(s.mu);
    // (pending from an earlier frame: that frame had no upscale's dispatch after it. Not this frame's)
    if (s.pre_pending && texture_frame_no() != s.pre_frame && color0 == s.pre_scene) s.pre_pending = false;
    if (s.pre_pending) {   // (the engine drew already: a draw into the scene now is one it did not see)
        if (color0 == s.pre_scene) {
            s.pre_touched = true;
            if (s.pre_late++ < 24)
                logf("vk: upscaler: program %016llx draws into the scene after the engine drew before the bloom (frame %llu)",
                     (unsigned long long)program, (unsigned long long)texture_frame_no());
            // (the first frames it happens in, 60 frames apart: the caller dumps the next frame, to see what they draw)
            const uint64_t now = texture_frame_no();
            if (late && s.pre_late_dumps < 3 && now - s.pre_late_frame > 60) *late = true, s.pre_late_frame = now, s.pre_late_dumps++;
        }
        return false;
    }
    if (!s.cam.valid || !s.cam.color || texture_frame_no() - s.cam.frame > 1) return false;
    if (color0 == s.cam.color) return s.pre_candidate = {}, false;   // (the scene still being drawn)
    const State::PrePoint here{program, color0->format, color0->width, color0->height};
    if (!s.pre_candidate.program) s.pre_candidate = here;
    // (the point only as the first draw after the scene's last: a frame whose first is another (the point learnt from a
    // frame without the bloom, whose first was the tone mapping) is left to the dispatch, which learns this frame's)
    return here == s.pre_key && s.pre_candidate == here;
}

Texture* temporal_pre_tonemap(VkCommandBuffer cmd) {
    const int mode = temporal_mode();
    if (!pre_wanted(mode)) return nullptr;
    State& s = st();
    std::lock_guard lk(s.mu);
    const Camera cam = s.cam;
    Texture* scene = const_cast<Texture*>(cam.color);
    // (AMD FSR 3.1: not one of the chain's engines, its own inputs and passes: pre_fsr3)
    const bool f3 = !ur::is_dlss(mode) && !ur::is_fsr4(mode);
    Engine* const engine = f3 ? nullptr : &engine_of(mode);
    // (anything unexpected: nothing here, and the upscale's dispatch draws as before, on the tone mapped picture)
    if (!cam.valid || !scene || (engine && (!engine->e || engine->failed)) || cam.depth_gen != texture_destroy_generation())
        return nullptr;
    const uint32_t rw = cam.width, rh = cam.height;
    const VkImageUsageFlags need = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    // (an RGBA16F scene too (local, RenoDX port): the engines' picture goes back into it through the pass's -DHDR layout.
    // AMD FSR 3.1's own output pass has only the plain one)
    if (f3 && scene->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32) return nullptr;
    if (scene->width != rw || scene->height != rh || !temporal_scene_format(scene->format) ||
        (scene->usage & need) != need || scene->samples != VK_SAMPLE_COUNT_1_BIT || scene->layers != 1) {
        static bool told;
        if (!told)
            told = true, logf("vk: upscaler: the scene's colour target cannot be drawn into before the bloom (%ux%u, format %d, "
                              "usage %#x): the engine draws on the tone mapped picture", scene->width, scene->height,
                              (int)scene->format, (unsigned)scene->usage);
        return nullptr;
    }
    if (cam.depth->width != rw || cam.depth->height != rh || !(cam.depth->usage & VK_IMAGE_USAGE_SAMPLED_BIT) ||
        cam.depth->samples != VK_SAMPLE_COUNT_1_BIT || !(cam.depth->aspect & VK_IMAGE_ASPECT_DEPTH_BIT))
        return nullptr;
    // (an engine that scales up draws at the upscale's output size, as last seen)
    uint32_t ow = rw, oh = rh;
    if (!ur::full_scale(mode)) {
        ow = s.out_w.load(std::memory_order_relaxed), oh = s.out_h.load(std::memory_order_relaxed);
        if (!ow || !oh || !dlss_checks::scene_size_fits(rw, rh, ow, oh)) return nullptr;
    }
    const VkImageView view = depth_view(scene);   // (its colour, sampled by the inputs pass and written by the output pass)
    if (!view) return nullptr;
    free_dead(false);
    s.cam.valid = false;   // (each frame's own camera)
    struct SkinsDone {
        State& s;
        ~SkinsDone() { s.skins.clear(); }
    } skins_done{s};
    const std::vector<State::Captured> captures = take_captures();
    s.asked = texture_frame_no(), s.holding = true;
    if (f3) return pre_fsr3(cmd, scene, view, rw, rh, ow, oh) ? scene : nullptr;
    if (!chain(cmd, mode, *engine, cam, captures, view, rw, rh, ow, oh, view, scene, scene->format, scene->format)) return nullptr;
    return scene;
}

bool temporal_pre_dispatch(VkCommandBuffer cmd, Texture* in, VkImageView in_view, Texture* out, VkImageView out_view,
                           uint32_t out_level, bool learn) {
    const int mode = temporal_mode();
    if (!pre_wanted(mode)) return false;
    State& s = st();
    std::lock_guard lk(s.mu);
    if ((learn || s.pre_pending) && out && temporal_scene_format(out->format) && in &&
        (out->width >> out_level) >= in->width)
        note_output(out, out_level);
    return pre_dispatch(cmd, mode, in, in_view, out, out_view, out_level, learn);
}

bool temporal_pre_composite(const Texture* color0, uint32_t width, uint32_t height, TemporalRetarget* k) {
    State& s = st();
    std::lock_guard lk(s.mu);
    if (!s.pre_pending || s.pre_final || !s.pre_scene || !color0 || color0 == s.pre_scene) return false;
    if (!temporal_scene_format(color0->format) || width != s.pre_scene->width || height != s.pre_scene->height)
        return false;
    s.pre_final = color0;
    // (the engine's picture is larger than the scene: the draw once more, into the upscale's output at its size)
    Texture* out = s.pre_out;
    const auto& pic = s.pre_picture;
    if (!pic.view || (pic.width == width && pic.height == height) || !out || s.pre_out_gen != texture_destroy_generation())
        return false;
    if (out->width != pic.width || out->height != pic.height || !temporal_scene_format(out->format) ||
        !(out->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) || out->samples != VK_SAMPLE_COUNT_1_BIT || out->layers != 1)
        return false;
    const VkImageView view = depth_view(out);
    if (!view) return false;
    k->target = out, k->target_view = view, k->width = out->width, k->height = out->height;
    k->from = s.pre_scene, k->to_view = pic.view;
    return true;
}

VkImageView temporal_pre_merged(VkCommandBuffer cmd) {
    State& s = st();
    std::lock_guard lk(s.mu);
    const uint32_t ow = s.pre_picture.width, oh = s.pre_picture.height;
    // (nothing drawn into the scene since, or no copy of it: the engine's picture as it is)
    if (!s.pre_pending || !s.pre_touched || !s.pre_scene || !s.base.image || s.base.width != s.pre_scene->width ||
        s.base.height != s.pre_scene->height)
        return s.pre_picture.view;
    const VkDescriptorType S = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, I = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    const VkDescriptorType types[4] = {S, S, S, I};
    if (!s.merge && !make_pipeline(DLSS_MERGE_COMP_SPV, sizeof DLSS_MERGE_COMP_SPV, types, 4, 8, s.merge_set, s.merge_layout, s.merge))
        return s.pre_picture.view;
    const VkImageView scene = depth_view(const_cast<Texture*>(s.pre_scene));
    if (!scene || !make_image(cmd, s.merged, VK_FORMAT_R16G16B16A16_SFLOAT, ow, oh)) return s.pre_picture.view;
    const int32_t pc[2] = {(int32_t)ow, (int32_t)oh};
    const VkDescriptorImageInfo info[4] = {{s.linear, scene, VK_IMAGE_LAYOUT_GENERAL},
                                           {s.linear, s.base.view, VK_IMAGE_LAYOUT_GENERAL},
                                           {s.point, s.pre_picture.view, VK_IMAGE_LAYOUT_GENERAL},
                                           {VK_NULL_HANDLE, s.merged.view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet w[4]{};
    for (uint32_t i = 0; i < 4; i++) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstBinding = i, w[i].descriptorCount = 1, w[i].pImageInfo = &info[i];
        w[i].descriptorType = i < 3 ? S : I;
    }
    full_barrier(cmd);   // (the scene, drawn into just before)
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.merge);
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.merge_layout, 0, 4, w);
    vkCmdPushConstants(cmd, s.merge_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cmd, (ow + 7) / 8, (oh + 7) / 8, 1);
    full_barrier(cmd);
    descriptors_disturbed();
    static bool told;
    if (!told) told = true, logf("vk: upscaler: what the game draws into the scene after the engine's pass (the lens flare) is "
                                 "added to the engine's picture (frame %llu)", (unsigned long long)texture_frame_no());
    return s.merged.view;
}

void temporal_pre_composited(bool drawn) {
    State& s = st();
    std::lock_guard lk(s.mu);
    s.pre_direct = drawn;
    static bool told;
    if (drawn && !told)
        told = true, logf("vk: upscaler: the game's tone mapping draws at the output's size from the engine's picture (%ux%u), "
                          "into the upscale's output", s.pre_picture.width, s.pre_picture.height);
}


} // namespace mkwvk
