// Vulkan Memory Allocator (v3.3.0, from Eglantine's CPM cache: nx2/build.py adds its include path): images and
// device buffers are sub-allocated from large blocks (NVIDIA allows ~4096 allocations, and the imported pools
// already take about 2400 of them). Its Vulkan functions come from vkGetInstanceProcAddr / vkGetDeviceProcAddr.
#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include "vk_internal.h"
#include <vk_mem_alloc.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "vk_work.h"   // (last: RX_VK_RECORDER=defer drains before the destructions and the invalidate below)

namespace mkwvk {

VmaAllocator vma;
uint64_t test_card_ballast = 0;   // (RX_VK_TEST_CARD_MB, a lab tool: the VRAM taken so that the card plays a smaller one; vma_init)

bool vma_init(PFN_vkGetInstanceProcAddr gipa) {
    VmaVulkanFunctions f{};
    f.vkGetInstanceProcAddr = gipa;
    f.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo ci{};
    ci.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT |
               (dev.memory_budget ? VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT : 0);
    ci.physicalDevice = dev.physical;
    ci.device = dev.device;
    ci.instance = dev.instance;
    ci.vulkanApiVersion = VK_API_VERSION_1_3;
    ci.pVulkanFunctions = &f;
    ci.preferredLargeHeapBlockSize = 64ull << 20;   // (64 MB blocks: Eglantine found 256 MB ones wasted VRAM)
    if (vmaCreateAllocator(&ci, &vma) != VK_SUCCESS) return false;
    // (Triforce, a lab tool) RX_VK_TEST_BALLAST_MB=<n>: that much VRAM taken and held for the session, in 256 MB device buffers
    // of their own. RX_VK_TEST_ALLOWED_MB only changes the number the budget is told; this fills the card for real, as a card
    // with less VRAM or another program's use does: the driver's own behaviour at its limit (allocations of 110-170 ms in a
    // player's log) only shows then
    // (Triforce, 11-12, a lab tool) RX_VK_TEST_CARD_MB=<n>: a card of n MB played on a larger one. The rest of the card is taken
    // as ballast, and neither what the process uses nor what the driver allows counts it (vram_usage, device_vram_bytes), so
    // every rule that goes by the card's size or by the share used sees the small card, and the driver is as short as there.
    // With RX_VK_TEST_BALLAST_MB alone the rules still saw the large card (the textures' budget 70% of it, the mirrors' 3 GB)
    const char* card = getenv("RX_VK_TEST_CARD_MB");
    uint64_t card_ballast = 0;
    if (card) {
        uint64_t heap = 0;
        for (uint32_t h = 0; h < dev.memory.memoryHeapCount; h++)
            if (dev.memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) heap = std::max(heap, (uint64_t)dev.memory.memoryHeaps[h].size);
        const uint64_t n = (uint64_t)std::max(0, atoi(card)) << 20;
        card_ballast = heap > n ? heap - n : 0;
    }
    if (const char* e = card ? card : getenv("RX_VK_TEST_BALLAST_MB")) {
        const uint64_t want = card ? card_ballast : (uint64_t)std::max(0, atoi(e)) << 20, chunk = 256ull << 20;
        uint64_t got = 0;
        for (; got < want; got += chunk) {
            VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bi.size = std::min(chunk, want - got);
            bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            VmaAllocationCreateInfo ai{};
            ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            ai.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            ai.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
            VkBuffer b = VK_NULL_HANDLE;
            VmaAllocation a = VK_NULL_HANDLE;
            if (vmaCreateBuffer(vma, &bi, &ai, &b, &a, nullptr) != VK_SUCCESS) break;   // (kept: never freed)
        }
        if (card) test_card_ballast = got;
        logf("vk: lab: %llu MB of VRAM taken as ballast (%s)", (unsigned long long)(got >> 20),
             card ? "RX_VK_TEST_CARD_MB: not counted, the card plays a smaller one" : "RX_VK_TEST_BALLAST_MB");
    }
    return true;
}

// Images come from a pool of 128 MB blocks: on their own, VMA gave most of them dedicated memory (the driver prefers it
// for render targets, and anything over half a default block), a vkAllocateMemory and a kernel call for each, and
// the dynamic resolution makes a new set of render targets every few frames. Images too large for a block, or of a
// memory type the pool's is not, are allocated as before.
VmaPool image_pool = VK_NULL_HANDLE;
bool image_pool_tried = false;
// (RX_VK_IMAGE_BLOCK_MB=<n>, 16 and up: the block's size. VMA keeps one block empty for the next image, 128 MB of VRAM that a 64 MB block
// would halve, at twice the memory objects: the driver allows ~4096, the imported pools take ~2400)
const uint64_t kImageBlock = (getenv("RX_VK_IMAGE_BLOCK_MB") ? (uint64_t)std::max(16, atoi(getenv("RX_VK_IMAGE_BLOCK_MB"))) : 128u) << 20;

// (hookshot) and the render targets' images in blocks of their own (texture_get's for a target: OoT's transient ones, made and let go
// all the time). In the same blocks as the textures that stay, each one gone left a hole beside a texture that kept its block: the
// pool's 47-54 blocks (6-6.9 GB) held 1.9-3.6 GB of images in Kokiri Forest, VRAM sat at 93-98% of what the driver allows, and an
// image made there then took 30-70 ms (RTX 5070). RX_VK_TARGET_BLOCKS=0: in the textures' blocks, as before
VmaPool target_image_pool = VK_NULL_HANDLE;
bool target_pool_tried = false;
size_t target_floor_blocks = 0;
// (Triforce, 11-12) RX_VK_TARGET_BLOCK_MB=<n>, 16 and up: the size of the render targets' blocks alone (the textures' and the
// other images' stay at RX_VK_IMAGE_BLOCK_MB)
uint64_t target_block() {
    static const uint64_t b = getenv("RX_VK_TARGET_BLOCK_MB") ? (uint64_t)std::max(16, atoi(getenv("RX_VK_TARGET_BLOCK_MB"))) << 20 : kImageBlock;
    return b;
}
bool target_floor_kept() { return target_floor_blocks != 0; }
VmaPool images_pool();
VmaPool target_images_pool() {
    if (target_pool_tried) return target_image_pool;
    target_pool_tried = true;
    static const bool off = getenv("RX_VK_TARGET_BLOCKS") && !atoi(getenv("RX_VK_TARGET_BLOCKS"));
    if (off || !images_pool()) return VK_NULL_HANDLE;
    VkImageCreateInfo rep{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};   // (the image pool's representative image: the same memory type)
    rep.imageType = VK_IMAGE_TYPE_2D;
    rep.format = VK_FORMAT_R8G8B8A8_UNORM;
    rep.extent = {256, 256, 1};
    rep.mipLevels = rep.arrayLayers = 1;
    rep.samples = VK_SAMPLE_COUNT_1_BIT;
    rep.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    ai.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    uint32_t type = 0;
    if (vmaFindMemoryTypeIndexForImageInfo(vma, &rep, &ai, &type) != VK_SUCCESS) return VK_NULL_HANDLE;
    VmaPoolCreateInfo pi{};
    pi.memoryTypeIndex = type;
    pi.blockSize = target_block();
    // (Triforce, 11-11) A floor of blocks the pool keeps for the session (RX_VK_TARGET_FLOOR_MB; 0: none, as before). While the
    // camera moves the game makes its transient targets at new sizes all the time (a player's session: up to 6733 new images in
    // 600 presents, 11 a frame), their blocks emptied and filled again; VMA keeps one empty block and frees the rest, and at the
    // driver's limit (its budget reached) not even that one: every block the pool needed again was a new 128 MB from the
    // driver, 110-170 ms each there. That session had ten frames of 115-180 ms in a row from it, VRAM climbing 9.5 -> 10.8 GB
    // of 10.5 allowed. Below the floor VMA never gives a block back, so nothing is asked of the driver in play
    static const uint64_t floor_mb = getenv("RX_VK_TARGET_FLOOR_MB") ? (uint64_t)std::max(0, atoi(getenv("RX_VK_TARGET_FLOOR_MB"))) : 0;
    pi.minBlockCount = (size_t)((floor_mb << 20) / target_block());
    target_floor_blocks = pi.minBlockCount;
    if (vmaCreatePool(vma, &pi, &target_image_pool) != VK_SUCCESS) {
        pi.minBlockCount = 0;   // (no room for the floor now: the pool as before)
        target_floor_blocks = 0;
        if (vmaCreatePool(vma, &pi, &target_image_pool) != VK_SUCCESS) target_image_pool = VK_NULL_HANDLE;
    }
    logf("vk: render targets' images from blocks of their own (%llu MB, memory type %u; %zu blocks kept for the session)%s",
         (unsigned long long)(target_block() >> 20), type, pi.minBlockCount, target_image_pool ? "" : ": not made");
    return target_image_pool;
}

VmaPool images_pool() {
    if (image_pool_tried) return image_pool;
    image_pool_tried = true;
    VkImageCreateInfo rep{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};   // (a representative image: the pool's memory type)
    rep.imageType = VK_IMAGE_TYPE_2D;
    rep.format = VK_FORMAT_R8G8B8A8_UNORM;
    rep.extent = {256, 256, 1};
    rep.mipLevels = rep.arrayLayers = 1;
    rep.samples = VK_SAMPLE_COUNT_1_BIT;
    rep.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    ai.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    uint32_t type = 0;
    if (vmaFindMemoryTypeIndexForImageInfo(vma, &rep, &ai, &type) != VK_SUCCESS) return VK_NULL_HANDLE;
    VmaPoolCreateInfo pi{};
    pi.memoryTypeIndex = type;
    pi.blockSize = kImageBlock;
    if (vmaCreatePool(vma, &pi, &image_pool) != VK_SUCCESS) image_pool = VK_NULL_HANDLE;
    logf("vk: images from a pool of %llu MB blocks (memory type %u)%s", (unsigned long long)(kImageBlock >> 20), type,
         image_pool ? "" : ": not made");
    return image_pool;
}

// (hookshot) The sampled textures' own pool, when they are compacted (unless RX_VK_COMPACT=0): VMA's defragmentation asks to move any
// allocation of the pool it works on, and only vk_texture.cpp's images can be moved (it knows what they hold and who uses
// them) or kept from being freed while a pass is open. The render targets' images stay in their blocks above; DLSS's, the
// frame interpolation's and the scratch ones in the images' pool
VmaPool texture_pool = VK_NULL_HANDLE;
bool texture_pool_tried = false;
VmaPool textures_pool() {
    if (texture_pool_tried) return texture_pool;
    texture_pool_tried = true;
    VkImageCreateInfo rep{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};   // (the image pool's representative image: the same memory type)
    rep.imageType = VK_IMAGE_TYPE_2D;
    rep.format = VK_FORMAT_R8G8B8A8_UNORM;
    rep.extent = {256, 256, 1};
    rep.mipLevels = rep.arrayLayers = 1;
    rep.samples = VK_SAMPLE_COUNT_1_BIT;
    rep.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    ai.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    uint32_t type = 0;
    if (vmaFindMemoryTypeIndexForImageInfo(vma, &rep, &ai, &type) != VK_SUCCESS) return VK_NULL_HANDLE;
    VmaPoolCreateInfo pi{};
    pi.memoryTypeIndex = type;
    pi.blockSize = kImageBlock;
    if (vmaCreatePool(vma, &pi, &texture_pool) != VK_SUCCESS) texture_pool = VK_NULL_HANDLE;
    logf("vk: textures from a pool of their own, %llu MB blocks (RX_VK_COMPACT)%s", (unsigned long long)(kImageBlock >> 20),
         texture_pool ? "" : ": not made");
    return texture_pool;
}
bool textures_pool_usage(uint64_t* block_bytes, uint64_t* free_bytes) {
    if (!texture_pool) return false;
    VmaStatistics st{};
    vmaGetPoolStatistics(vma, texture_pool, &st);
    *block_bytes = st.blockBytes, *free_bytes = st.blockBytes - std::min(st.blockBytes, st.allocationBytes);
    return true;
}

// ---- the textures' pool compacted: VMA's defragmentation context and its open pass ----
namespace {
VmaDefragmentationContext compact_ctx = VK_NULL_HANDLE;
VmaDefragmentationPassMoveInfo compact_pass{};
bool compact_in_pass = false;
bool compact_over(uint64_t* freed_bytes, uint32_t* freed_blocks) {
    VmaDefragmentationStats st{};
    vmaEndDefragmentation(vma, compact_ctx, &st);
    compact_ctx = VK_NULL_HANDLE;
    if (freed_bytes) *freed_bytes = st.bytesFreed;
    if (freed_blocks) *freed_blocks = st.deviceMemoryBlocksFreed;
    return false;
}
}
bool image_compact_begin(uint64_t max_bytes_a_pass, uint32_t max_moves_a_pass) {
    if (compact_ctx || !texture_pool) return false;
    VmaDefragmentationInfo di{};
    di.pool = texture_pool;
    di.maxBytesPerPass = max_bytes_a_pass;
    di.maxAllocationsPerPass = max_moves_a_pass;
    return vmaBeginDefragmentation(vma, &di, &compact_ctx) == VK_SUCCESS;
}
bool image_compact_active() { return compact_ctx != VK_NULL_HANDLE; }
bool image_compact_pass(std::vector<ImageMove>& moves, uint64_t* freed_bytes, uint32_t* freed_blocks) {
    moves.clear();
    if (!compact_ctx || compact_in_pass) return false;
    const VkResult r = vmaBeginDefragmentationPass(vma, compact_ctx, &compact_pass);
    if (r != VK_INCOMPLETE) return compact_over(freed_bytes, freed_blocks);   // (VK_SUCCESS: nothing left to move; or an error)
    compact_in_pass = true;
    for (uint32_t i = 0; i < compact_pass.moveCount; i++) moves.push_back({compact_pass.pMoves[i].srcAllocation});
    return true;
}
bool image_compact_bind(size_t i, VkImage image) {
    return compact_in_pass && i < compact_pass.moveCount &&
           vmaBindImageMemory(vma, compact_pass.pMoves[i].dstTmpAllocation, image) == VK_SUCCESS;
}
void image_compact_ignore(size_t i) {
    if (compact_in_pass && i < compact_pass.moveCount)
        compact_pass.pMoves[i].operation = VMA_DEFRAGMENTATION_MOVE_OPERATION_IGNORE;
}
bool image_compact_pass_end(uint64_t* freed_bytes, uint32_t* freed_blocks) {
    if (!compact_ctx || !compact_in_pass) return false;
    compact_in_pass = false;
    if (vmaEndDefragmentationPass(vma, compact_ctx, &compact_pass) == VK_INCOMPLETE) return true;
    return compact_over(freed_bytes, freed_blocks);
}

bool create_image_inner(const VkImageCreateInfo& ci, VkImage& image, VmaAllocation& allocation, bool dedicated, bool target, bool textures);
thread_local int create_image_path = 0;   // (the last create_image_inner: 0 its pool or a dedicated one, 1 the pool refused, 2 system memory)
/// (Triforce) An image that takes over 3 ms to make: what it was, how long, and whether its pool took a new block for it (60
/// lines). A player's session had frames of 50 to 150 ms in play that were all "texture lookups", 24 render targets made in one
bool create_image(const VkImageCreateInfo& ci, VkImage& image, VmaAllocation& allocation, bool dedicated, bool target, bool textures) {
    const auto blocks = [&]() -> uint32_t {
        VmaPool pool = textures ? texture_pool : target ? target_image_pool : image_pool;
        if (!pool) return 0;
        VmaStatistics st{};
        vmaGetPoolStatistics(vma, pool, &st);
        return st.blockCount;
    };
    const uint32_t before = blocks();
    const auto t0 = std::chrono::steady_clock::now();
    create_image_path = 0;
    const bool ok = create_image_inner(ci, image, allocation, dedicated, target, textures);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    {   // (each 600 target images: how many the pool refused, and what it holds)
        static std::atomic<int64_t> targets{0}, refused{0}, new_blocks{0};
        if (target) {
            refused += create_image_path != 0;
            new_blocks += blocks() > before;
            if (++targets % 600 == 0 && target_image_pool) {
                VmaStatistics st{};
                vmaGetPoolStatistics(vma, target_image_pool, &st);
                logf("vk: render targets' blocks: %u blocks %llu MB hold %u images %llu MB; of %lld target images so far %lld took a "
                     "new block and %lld were refused by the pool", st.blockCount, (unsigned long long)(st.blockBytes >> 20),
                     st.allocationCount, (unsigned long long)(st.allocationBytes >> 20), (long long)targets.load(),
                     (long long)new_blocks.load(), (long long)refused.load());
            }
        }
    }
    static std::atomic<int> told{0};
    static std::atomic<int64_t> n{0}, slow{0};
    static std::atomic<int64_t> slow_us{0};
    n++;
    if (ms > 3.0) {
        slow++, slow_us += (int64_t)(ms * 1000);
        if (told++ < 60) {
            uint64_t used = 0, allowed = 0, block_free = 0;
            vram_usage(&used, &allowed, &block_free);
            logf("vk: an image took %.1f ms to make: %ux%u x%u levels %u format %d (%s)%s; its pool's blocks %u -> %u; VRAM %llu of %llu "
                 "MB, %llu MB free in the blocks; %lld of %lld images so far took over 3 ms (%.0f ms in all)", ms, ci.extent.width,
                 ci.extent.height, ci.arrayLayers, ci.mipLevels, (int)ci.format, textures ? "a texture" : target ? "a render target" : "other",
                 !ok ? ", NOT made" : create_image_path ? ", outside its pool" : "", before, blocks(), (unsigned long long)(used >> 20),
                 (unsigned long long)(allowed >> 20),
                 (unsigned long long)(block_free >> 20), (long long)slow.load(), (long long)n.load(), slow_us.load() / 1000.0);
        }
    }
    return ok;
}
// ---- (Triforce, 11-12) a block made ahead ----
// A pool's new block is a vkAllocateMemory on the thread that asked for the image, the replay thread: 37 to 55 ms each for the
// textures' pool in a player's session on an RTX 4070 SUPER (four of them inside the frames of an area's arrival, VRAM at 26 to
// 54% of what the driver allows), 18 to 63 ms for a mirror's piece, and 110 to 170 ms near the driver's limit. VMA keeps one
// empty block for the next image, but lets it go at the next free of anything else in the pool while it is the last block, so
// with images coming and going there is seldom one. Here each pool holds a block of its own in reserve: an allocation of a whole
// block, made on a thread of its own, that nothing uses. An image (or a piece) first asks the pool for room in the blocks it
// has (VMA_ALLOCATION_CREATE_NEVER_ALLOCATE); with none, the reserve is let go, which leaves its block empty, the image goes
// there, and the thread makes the next reserve. No reserve is made that would put VRAM over 88% of what the driver allows, and
// the ones held go back over 93% (vma_blocks_ahead_check, every 600 presents): a card that is short keeps nothing aside. What
// is held is left out of vram_usage's count: the rules that go by the share used (the textures' budget, the compaction's
// hold) see the VRAM as it is without the reserves, which go the moment anything needs them.
// Only where the driver allows the process 9 GB and more (cards of more than 8 GB): a card played at 5.6 GB allowed
// (RX_VK_TEST_CARD_MB=7168, three tours against two) had 559-896 frames over 40 ms with this and the let-go rule against
// 369-614 without, and flat textures on screen for 6.1-7.4 s against 3.9-4.3: there every block held aside is missed.
// RX_VK_BLOCK_AHEAD=0: off, as before; =1: on whatever the driver allows
namespace {
struct Ahead {
    std::atomic<VmaAllocation> held{nullptr};
    std::atomic<bool> asked{false};
    uint64_t block = 0;
    const char* what = "";
};
Ahead ahead_targets, ahead_textures, ahead_images, ahead_pieces;
uint64_t ahead_held_bytes() {
    uint64_t n = 0;
    for (const Ahead* a : {&ahead_targets, &ahead_textures, &ahead_images, &ahead_pieces})
        if (a->held.load(std::memory_order_relaxed)) n += a->block;
    return n;
}
std::mutex ahead_mu;
std::condition_variable ahead_cv;
std::deque<std::pair<VmaPool, Ahead*>> ahead_queue;
std::atomic<int64_t> st_ahead_made{0}, st_ahead_used{0}, st_ahead_none{0}, st_ahead_back{0};
int ahead_mode() {   // (0 off, 1 on, 2 by what the driver allows: the default)
    static const int m = getenv("RX_VK_BLOCK_AHEAD") ? (atoi(getenv("RX_VK_BLOCK_AHEAD")) != 0 ? 1 : 0) : 2;
    return m;
}
bool ahead_on() { return ahead_mode() != 0; }
constexpr uint64_t kAheadMinAllowed = 9ull << 30;
void ahead_main() {
    for (;;) {
        std::pair<VmaPool, Ahead*> job;
        {
            std::unique_lock lk(ahead_mu);
            ahead_cv.wait(lk, [] { return !ahead_queue.empty(); });
            job = ahead_queue.front();
            ahead_queue.pop_front();
        }
        Ahead& a = *job.second;
        uint64_t used = 0, allowed = 0;
        const bool known = vram_usage(&used, &allowed, nullptr);
        used += ahead_held_bytes();   // (vram_usage leaves them out)
        if (a.held.load() || (known && used + a.block > allowed / 100 * 88) ||   // (one there, or the card has no room to keep one,
            (ahead_mode() == 2 && (!known || allowed < kAheadMinAllowed))) {       // or is a small one)
            a.asked = false;
            continue;
        }
        VkMemoryRequirements req{};
        req.size = a.block, req.alignment = 4096, req.memoryTypeBits = ~0u;
        VmaAllocationCreateInfo ai{};
        ai.pool = job.first;
        if (dev.memory_budget) ai.flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
        VmaAllocation r = VK_NULL_HANDLE;
        const auto t0 = std::chrono::steady_clock::now();
        if (vmaAllocateMemory(vma, &req, &ai, &r, nullptr) == VK_SUCCESS) {
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            a.held = r;
            const int64_t n = ++st_ahead_made;
            if (n <= 6 || n % 32 == 0)
                logf("vk: a %llu MB block made ahead for %s in %.1f ms, off the replay thread (%lld made, %lld used, %lld times none "
                     "was there, %lld given back; RX_VK_BLOCK_AHEAD=0: off)", (unsigned long long)(a.block >> 20), a.what, ms,
                     (long long)n, (long long)st_ahead_used.load(), (long long)st_ahead_none.load(), (long long)st_ahead_back.load());
        }
        a.asked = false;
    }
}
void ahead_ask(VmaPool pool, Ahead& a) {
    if (a.held.load() || a.asked.exchange(true)) return;
    std::lock_guard lk(ahead_mu);
    static bool started;
    if (!started) {
        started = true;
        std::thread(ahead_main).detach();
    }
    ahead_queue.push_back({pool, &a});
    ahead_cv.notify_one();
}
/// The pool's block made ahead let go, so that the allocation that found no room goes into it; false: none was there
bool ahead_take(VmaPool pool, Ahead& a) {
    VmaAllocation r = compact_in_pass && pool == texture_pool ? VK_NULL_HANDLE : a.held.exchange(nullptr);   // (an open pass may list it)
    if (r) vmaFreeMemory(vma, r), st_ahead_used++;
    else st_ahead_none++;
    ahead_ask(pool, a);
    return r != VK_NULL_HANDLE;
}
Ahead* ahead_of(VmaPool pool) {
    if (!ahead_on() || !pool) return nullptr;
    Ahead* a = pool == texture_pool ? &ahead_textures : pool == target_image_pool ? &ahead_targets : pool == image_pool ? &ahead_images : nullptr;
    if (a && !a->block)
        a->block = a == &ahead_targets ? target_block() : kImageBlock, a->what = a == &ahead_textures ? "the textures" : a == &ahead_targets ? "the render targets" : "the other images";
    return a;
}
}   // namespace
void vma_blocks_ahead_check() {
    if (!ahead_on()) return;
    uint64_t used = 0, allowed = 0;
    if (!ahead_held_bytes() || !vram_usage(&used, &allowed, nullptr) || used + ahead_held_bytes() <= allowed / 100 * 93) return;
    for (Ahead* a : {&ahead_targets, &ahead_textures, &ahead_images, &ahead_pieces}) {
        if (a == &ahead_textures && compact_in_pass) continue;
        if (VmaAllocation r = a->held.exchange(nullptr)) vmaFreeMemory(vma, r), st_ahead_back++;
    }
}
bool create_image_inner(const VkImageCreateInfo& ci, VkImage& image, VmaAllocation& allocation, bool dedicated, bool target, bool textures) {
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    // (in VRAM or not at all: an image VMA puts in system memory when VRAM is full is read over PCIe every frame,
    // and takes the RAM the game and everything else on the machine need)
    ai.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    if (dev.memory_budget) ai.flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
    if (dedicated) {   // (a memory object of its own, which freeing gives back whole: a pool's block is kept by VMA when it empties)
        VmaAllocationCreateInfo dai = ai;
        dai.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        if (vmaCreateImage(vma, &ci, &dai, &image, &allocation, nullptr) == VK_SUCCESS) return true;
    }
    static const bool no_pool = getenv("RX_VK_NO_IMAGE_POOL") != nullptr;
    if (VmaPool pool = no_pool ? VK_NULL_HANDLE
                       : textures && textures_pool() ? textures_pool()
                       : target && target_images_pool() ? target_images_pool() : images_pool()) {
        VmaAllocationCreateInfo pai = ai;
        pai.pool = pool;
        pai.usage = VMA_MEMORY_USAGE_UNKNOWN;
        if (Ahead* a = ahead_of(pool)) {   // (Triforce: in the blocks the pool has; else in the one made ahead: above)
            VmaAllocationCreateInfo nai = pai;
            nai.flags |= VMA_ALLOCATION_CREATE_NEVER_ALLOCATE_BIT;
            if (vmaCreateImage(vma, &ci, &nai, &image, &allocation, nullptr) == VK_SUCCESS) {
                ahead_ask(pool, *a);
                return true;
            }
            if (ahead_take(pool, *a) && vmaCreateImage(vma, &ci, &nai, &image, &allocation, nullptr) == VK_SUCCESS) return true;
        }
        if (vmaCreateImage(vma, &ci, &pai, &image, &allocation, nullptr) == VK_SUCCESS) return true;
        create_image_path = 1;   // (the pool refused it: the allocator's own blocks, or a memory object of its own)
    }
    if (vmaCreateImage(vma, &ci, &ai, &image, &allocation, nullptr) == VK_SUCCESS) return true;
    create_image_path = 2;
    // (hookshot) VRAM is full (a 4 GB card at a high resolution, #96): an image left unmade is a black screen, so it goes
    // to system memory instead, slow but drawn
    if (getenv("RX_VK_IMAGES_VRAM_ONLY")) return false;
    VmaAllocationCreateInfo sai{};
    sai.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(vma, &ci, &sai, &image, &allocation, nullptr) != VK_SUCCESS) return false;
    static std::atomic<int> told{0};
    if (told++ < 4) logf("vk: VRAM full: a %ux%u image (format %d) in system memory", ci.extent.width, ci.extent.height, (int)ci.format);
    return true;
}
void destroy_image(VkImage image, VmaAllocation allocation) {
    vmaDestroyImage(vma, image, allocation);
}
uint64_t allocation_size(VmaAllocation allocation) {
    VmaAllocationInfo info{};
    if (allocation) vmaGetAllocationInfo(vma, allocation, &info);
    return info.size;
}
bool create_buffer(uint64_t size, VkBufferUsageFlags usage, bool host_visible, VkBuffer& buffer,
                   VmaAllocation& allocation, void** mapped, bool dedicated) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    if (host_visible) ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    // (a buffer with VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT gets VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT from VMA:
    // vma_init sets VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT)
    static const bool blocks = getenv("RX_VK_BLOCK_BUFFERS") != nullptr;
    if (dedicated && !blocks) ai.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(vma, &bi, &ai, &buffer, &allocation, &info) != VK_SUCCESS) return false;
    if (mapped) *mapped = info.pMappedData;
    return true;
}
/// A buffer in cached host memory, mapped: written on one thread, read back on the CPU too (the captures: the game's
/// threads copy pages into it, the replay reads texture pages out of it and the GPU copies the rest from it)
bool create_buffer_cached(uint64_t size, VkBufferUsageFlags usage, VkBuffer& buffer, VmaAllocation& allocation,
                          void** mapped, bool dedicated) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    if (dedicated) ai.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;   // (its own memory object: no part of a 64 MB block left unused)
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(vma, &bi, &ai, &buffer, &allocation, &info) != VK_SUCCESS) return false;
    if (mapped) *mapped = info.pMappedData;
    return true;
}
bool host_cached(VmaAllocation allocation) {
    VkMemoryPropertyFlags f = 0;
    if (allocation) vmaGetAllocationMemoryProperties(vma, allocation, &f);
    return (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0;
}
void flush_mapped(VmaAllocation allocation, uint64_t size) {
    if (size) vmaFlushAllocation(vma, allocation, 0, size);
}
void flush_mapped_range(VmaAllocation allocation, uint64_t offset, uint64_t size) {
    if (size) vmaFlushAllocation(vma, allocation, offset, size);
}
void invalidate_mapped(VmaAllocation allocation, uint64_t size) {
    if (size) vmaInvalidateAllocation(vma, allocation, 0, size);
}
void destroy_buffer(VkBuffer buffer, VmaAllocation allocation) {
    vmaDestroyBuffer(vma, buffer, allocation);
}
bool create_device_buffer(uint64_t size, VkBufferUsageFlags usage, VkBuffer& buffer, VmaAllocation& allocation, bool dedicated) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    ai.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    if (dev.memory_budget) ai.flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
    if (dedicated) ai.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    return vmaCreateBuffer(vma, &bi, &ai, &buffer, &allocation, nullptr) == VK_SUCCESS;
}
/// (Triforce) Device memory for a piece of a sparse buffer (vk_memory.cpp's mirrors in pieces): size bytes at that alignment,
/// of a type in type_bits, within the driver's budget; the memory and the offset in it to bind
bool vma_piece_alloc(uint64_t size, uint64_t alignment, uint32_t type_bits, VmaAllocation& allocation, VkDeviceMemory& memory,
                     uint64_t& offset) {
    VkMemoryRequirements req{};
    req.size = size, req.alignment = alignment, req.memoryTypeBits = type_bits;
    VmaAllocationCreateInfo ai{};
    ai.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    if (dev.memory_budget) ai.flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
    VmaAllocationInfo info{};
    const auto t0 = std::chrono::steady_clock::now();
    // (Triforce, 11-12) from a pool of their own with a block made ahead (above), where one could be made for this memory type;
    // a piece larger than half a block, or one the pool has no room for and no block ahead, as before
    static VmaPool piece_pool = VK_NULL_HANDLE;
    static uint32_t piece_type = UINT32_MAX;
    static bool piece_pool_tried = false;
    constexpr uint64_t kPieceBlock = 64ull << 20;
    if (!piece_pool_tried && ahead_on()) {
        piece_pool_tried = true;
        if (vmaFindMemoryTypeIndex(vma, type_bits, &ai, &piece_type) == VK_SUCCESS) {
            VmaPoolCreateInfo pi{};
            pi.memoryTypeIndex = piece_type;
            pi.blockSize = kPieceBlock;
            if (vmaCreatePool(vma, &pi, &piece_pool) != VK_SUCCESS) piece_pool = VK_NULL_HANDLE;
        }
        ahead_pieces.block = kPieceBlock, ahead_pieces.what = "the mirrors' pieces";
    }
    bool ok = false;
    if (piece_pool && piece_type < 32 && (type_bits >> piece_type & 1) && size <= kPieceBlock / 2) {
        VmaAllocationCreateInfo nai = ai;
        nai.pool = piece_pool;
        nai.flags |= VMA_ALLOCATION_CREATE_NEVER_ALLOCATE_BIT;
        ok = vmaAllocateMemory(vma, &req, &nai, &allocation, &info) == VK_SUCCESS;
        if (ok) ahead_ask(piece_pool, ahead_pieces);
        else if (ahead_take(piece_pool, ahead_pieces)) ok = vmaAllocateMemory(vma, &req, &nai, &allocation, &info) == VK_SUCCESS;
        if (!ok) {   // (no block ahead: the pool takes a new one now, as the allocator's own blocks did)
            nai.flags &= ~(VmaAllocationCreateFlags)VMA_ALLOCATION_CREATE_NEVER_ALLOCATE_BIT;
            ok = vmaAllocateMemory(vma, &req, &nai, &allocation, &info) == VK_SUCCESS;
        }
    }
    if (!ok) ok = vmaAllocateMemory(vma, &req, &ai, &allocation, &info) == VK_SUCCESS;
    {   // (Triforce, 11-11: one that takes over 3 ms is named, 40 times: at the driver's limit an image's new block took
        // 110-170 ms, and submissions beside those were 130-150 ms of "memory sync" with nothing said of them)
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        static std::atomic<int> told{0};
        if (ms > 3.0 && told++ < 40) {
            uint64_t used = 0, allowed = 0, block_free = 0;
            vram_usage(&used, &allowed, &block_free);
            logf("vk: a mirror's piece took %.1f ms to get its memory (%llu KB%s); VRAM %llu of %llu MB", ms,
                 (unsigned long long)(size >> 10), ok ? "" : ", NOT given", (unsigned long long)(used >> 20),
                 (unsigned long long)(allowed >> 20));
        }
    }
    if (!ok) return false;
    memory = info.deviceMemory, offset = info.offset;
    return true;
}
void vma_piece_free(VmaAllocation allocation) {
    vmaFreeMemory(vma, allocation);
}
/// (every heap: what VMA has in blocks and allocations there, what the process uses of it, its budget; one line)
/// The image pools: their blocks, the images in them, and the free ranges between them (every 600 presents, with the heaps)
void vma_log_image_blocks() {
    for (VmaPool pool : {image_pool, target_image_pool, texture_pool}) {
        if (!pool) continue;
        VmaDetailedStatistics d{};
        vmaCalculatePoolStatistics(vma, pool, &d);
        logf("vk: image blocks%s: %u blocks %llu MB, %u images %llu MB; %u free ranges, the largest %llu MB",
             pool == target_image_pool ? " (render targets)" : pool == texture_pool ? " (textures, compacted)" : "", d.statistics.blockCount,
             (unsigned long long)(d.statistics.blockBytes >> 20), d.statistics.allocationCount,
             (unsigned long long)(d.statistics.allocationBytes >> 20), d.unusedRangeCount,
             (unsigned long long)(d.unusedRangeSizeMax >> 20));
    }
}
void vma_log_heaps() {
    VmaBudget b[VK_MAX_MEMORY_HEAPS]{};
    vmaGetHeapBudgets(vma, b);
    char line[1024];
    int n = snprintf(line, sizeof line, "vk: memory heaps:");
    for (uint32_t h = 0; h < dev.memory.memoryHeapCount && n < 900; h++)
        n += snprintf(line + n, sizeof line - n, " heap %u%s: %u blocks %llu MB, %u allocations %llu MB; used %llu of %llu MB;", h,
                      (dev.memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? " (device)" : " (system)",
                      b[h].statistics.blockCount, (unsigned long long)(b[h].statistics.blockBytes >> 20),
                      b[h].statistics.allocationCount, (unsigned long long)(b[h].statistics.allocationBytes >> 20),
                      (unsigned long long)(b[h].usage >> 20), (unsigned long long)(b[h].budget >> 20));
    logf("%s", line);
}
bool vram_usage(uint64_t* used, uint64_t* budget, uint64_t* block_free) {
    VmaBudget b[VK_MAX_MEMORY_HEAPS]{};
    vmaGetHeapBudgets(vma, b);
    uint64_t u = 0, t = 0, f = 0;
    for (uint32_t h = 0; h < dev.memory.memoryHeapCount; h++)
        if (dev.memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            u += b[h].usage, t += b[h].budget;
            // (the free space in VMA's blocks: the driver counts it as the process's, new allocations take it first)
            f += b[h].statistics.blockBytes - std::min(b[h].statistics.blockBytes, b[h].statistics.allocationBytes);
        }
    // (a test: RX_VK_TEST_ALLOWED_MB=<n> reports what the driver allows as at most n MB, so a 16 GB card plays the eviction and
    // keep-rendered paths an 8 GB card takes when its VRAM fills (the rainbow textures of issue 993 were never seen here otherwise))
    static const uint64_t test_allowed = getenv("RX_VK_TEST_ALLOWED_MB") ? (uint64_t)atoi(getenv("RX_VK_TEST_ALLOWED_MB")) << 20 : 0;
    if (test_card_ballast) u -= std::min(u, test_card_ballast), t -= std::min(t, test_card_ballast);   // (RX_VK_TEST_CARD_MB)
    u -= std::min(u, ahead_held_bytes());   // (Triforce: the blocks made ahead are nobody's until something needs them)
    if (test_allowed && t > test_allowed) t = test_allowed;
    *used = u, *budget = t;
    if (block_free) *block_free = f;
    return t != 0;
}

/// The system-memory heaps the GPU reaches: what the process uses there (the imported guest memory too: pinned by the
/// driver) and what the driver allows it, asked now (a new frame index makes VMA ask the driver again); false when unknown
namespace {
std::atomic<uint32_t> budget_frame{0};   // (VMA's frame index: a new one makes it ask the driver for the budgets again)
}
bool vram_usage_now(uint64_t* used, uint64_t* budget) {
    vmaSetCurrentFrameIndex(vma, ++budget_frame);
    return vram_usage(used, budget) && dev.memory_budget;
}
bool system_heap_usage(uint64_t* used, uint64_t* budget) {
    vmaSetCurrentFrameIndex(vma, ++budget_frame);
    VmaBudget b[VK_MAX_MEMORY_HEAPS]{};
    vmaGetHeapBudgets(vma, b);
    uint64_t u = 0, t = 0;
    for (uint32_t h = 0; h < dev.memory.memoryHeapCount; h++)
        if (!(dev.memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)) u += b[h].usage, t += b[h].budget;
    *used = u, *budget = t;
    return dev.memory_budget && t != 0;
}

} // namespace mkwvk
