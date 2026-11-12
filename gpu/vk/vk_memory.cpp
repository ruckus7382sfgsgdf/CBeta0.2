// Guest memory for the GPU: each NVN memory pool imported as host memory (VK_EXT_external_memory_host) with one
// buffer over it. The GPU reads the game's memory where it is and its writes land there: no mirrored copies, no
// write-backs, no upload streams. A GPU address (a guest address for the native pools) maps to (buffer, offset).
//
// Pools are imported the first time something on the GPU touches them, not all at once: MKW has ~2,400 pools at the
// title and importing each is a buffer, a memory object and a device address (tens of seconds taken at one submit,
// on the render thread, in the first frame). The table of pools is cheap to keep current (host->gpu_pools: two
// numbers each) and is refreshed when its generation changes, at a submit.
#include <cstring>
#include <algorithm>
#include <array>
#include <bit>   // (std::bit_ceil: Microsoft's library brings it in with <algorithm>, libc++ (the players' llvm-mingw) does not)
#include <chrono>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <vector>
#include <condition_variable>
#include "vk_menu.h"
#include <deque>
#include <unordered_map>
#include <thread>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "vk_internal.h"
#include "vk_texture.h"   // (upload_command_buffer)
#include "vk_gprof.h"      // (RX_VK_PASS_TIMING)
#include "import_hold.h"
#include "vk_paths.h"       // (game_module_path: triforce_mirror_keep.txt)
#include "mirror_inherit.h"
#ifdef __linux__
#include <cerrno>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif
#include "shaders/wb_diff_comp_spv.h"
#include "vram_pressure.h"
#include "vk_work.h"   // (after the others: counts the commands that do GPU work, for full_barrier)

namespace mkwvk { int64_t gpu_time_total_ns(); }   // (vk_replay.cpp)

namespace mkwvk {
namespace {

struct Pool {
    uint64_t lo = 0, hi = 0;   // guest range imported (aligned outwards)
    uint64_t range_lo = 0, range_hi = 0;   // (the game's pool range it was found for: pool_of's last hit)
    VkDeviceMemory memory = VK_NULL_HANDLE;
    std::vector<VkDeviceMemory> pieces;   // (a pool imported as several dma-bufs: the ones after `memory`)
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;
    // its VRAM mirror (see "the VRAM mirror" below)
    enum : uint8_t { MIRROR_NONE, MIRROR_WANTED, MIRROR_ON, MIRROR_REFUSED } mstate = MIRROR_NONE;
    VkBuffer mirror = VK_NULL_HANDLE;
    VmaAllocation mirror_alloc = nullptr;
    uint32_t synced_gen = 0;     // every page stamped up to this generation is in the mirror
    uint64_t synced_epoch = 0;   // the submission that last brought it up to date
    uint64_t sync_seg = 0;       // (and the parallel replay's segment whose uploads did)
    uint64_t last_value = 0;     // (the newest submission that bound it: its mirror is in use until that one is done)
    uint64_t direct_write_epoch = 0;   // (the part that last wrote it in guest memory, before it had a mirror)
    uint64_t retired_value = 0;  // (retired: the submission that may still use it)
    bool was_mirrored = false;   // (its mirror was given back: its pages stay in the captures, for the next one)
    // (EvilOOT) retired with its mirror on: the pool that took it in copies the GPU's contents from it when its own mirror is
    // made (mirror_make), and until then the mirror stays (mirrors_give_back)
    bool inherit_pending = false;
    uint64_t retired_epoch = 0;
    // (Evil Hookshot) the pages the GPU wrote in the mirror (bit per page), and the syncs that copied the CPU's version of
    // one of them over the GPU's: a pool both write every frame (OoT's GPU particles: the leaves, fireflies) leaves the mirrors
    std::vector<uint64_t> gpu_bits;
    uint32_t conflict_syncs = 0;
    bool demote_queued = false;
    bool demote_blocked = false;
    bool compute_writes = false;   // (a compute program's storage buffer wrote it: what a GPU simulation the CPU reads back does)   // (two trials cost GPU time: mirrored for good)
    uint8_t demote_fails = 0;
    uint32_t binds = 0, binds_at_sync = 0;   // (bindings, counted without the lock: approximate)
    float bind_rate = 0;                     // (bindings per sync, averaged)
    bool prefetched = false;     // (amd-fix) imported ahead by the background thread, not bound yet
    // (hookshot) the write-back's diff: what guest memory holds of the pool, in VRAM, and the pages of it known to be so
    VkBuffer shadow = VK_NULL_HANDLE;
    VmaAllocation shadow_alloc = nullptr;
    VkDeviceAddress shadow_address = 0;
    std::vector<uint64_t> shadow_known;   // (a bit a page)
    // (Triforce) its mirror in pieces (see "mirrors in pieces" below): a sparse buffer of the pool's size with device memory only
    // under the pieces a binding has reached. res_bits: a bit a piece, set once it has memory, is filled and the page table points
    // at it (read without the lock by gpu_buffer_bind: sized once for the pool's life, never freed before the pool)
    bool mpieces = false;
    uint32_t piece_shift = 0, piece_types = 0;
    uint64_t piece_align = 0, mirror_size = 0;   // (the sparse block, and the buffer's size rounded up to it)
    VkDeviceAddress mirror_address = 0;
    std::vector<uint64_t> res_bits;
    std::vector<VmaAllocation> res_allocs;
    uint64_t res_bytes = 0;
    // (the pieces left in guest memory because the CPU and the GPU both write pages of them (demote_trials): a bit a piece, read
    // without the lock as res_bits; conf_bits: the pieces a sync found such a page in, since the last trial; trial_bits: the
    // ones the trial under way took out)
    std::vector<uint64_t> direct_bits, conf_bits, trial_bits;
    bool direct_auto = false;   // (RX_VK_DEMOTE_PIECES=2: its trial left it out of the mirrors: such pieces leave as they are found)
};
uint64_t mem_rec_value = 0, mem_completed = 0;   // (the recording being made, and the GPU's progress)

struct GuestPool {
    uint64_t lo = 0, hi = 0;   // (aligned outwards the same way, so an import covers what the lookup found)
};

std::vector<GuestPool> guest_pools;   // by lo, from the host
std::atomic<uint64_t> guest_pools_gen{1};   // (bumped when that table is replaced: a span cached without the lock goes stale)
std::map<uint64_t, Pool> imported;    // by lo, made on first use
Pool* last_pool = nullptr;            // (the last pool_of hit: draws in a row bind buffers of the same pools)
extern Pool* pool_cache[4096];
void retire_pool(std::map<uint64_t, Pool>::iterator it);
Pool* pool_lookup(uint64_t guest, uint64_t size);
struct SrwMutex {   // (a slim reader/writer lock: half std::mutex's cost, taken for every buffer a draw binds)
    SRWLOCK l = SRWLOCK_INIT;
    void lock() { AcquireSRWLockExclusive(&l); }
    void unlock() { ReleaseSRWLockExclusive(&l); }
};
SrwMutex pools_mu;
uint32_t generation = ~0u;
uint64_t align = 4096;
uint64_t max_import_size = UINT64_MAX;
int failures = 0;
ImportHolds import_holds;          // (issue 218: a failed import is not tried again for ImportHolds::kGapMs)
std::mutex import_holds_mu;
thread_local bool import_cooling = false;
thread_local VkResult import_result = VK_SUCCESS;

int64_t import_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
bool import_on_hold(uint64_t lo, uint64_t hi) {
    std::lock_guard lk(import_holds_mu);
    import_cooling = !import_holds.allow(lo, hi, import_now_ms());
    return import_cooling;
}
void import_remember_failure(uint64_t lo, uint64_t hi) {
    std::lock_guard lk(import_holds_mu);
    import_holds.deny_for(lo, hi, import_now_ms());
}

constexpr VkBufferUsageFlags kUsage =
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
    VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;

#ifdef __linux__
// Guest memory handed to the GPU as a dma-buf over the game's own pages: the window is a memfd's (host/hos_main.c),
// and /dev/udmabuf makes a dma-buf of a range of it, which the device imports like any other. The pages are the same ones
// the game reads and writes (nothing is copied), pinned once when it is made. A host-pointer import (VK_EXT_external_memory_host)
// is a userptr buffer object to amdgpu, whose pages the kernel looks up again at every command submission, and RADV puts every
// buffer object in every submission: with ~1.5 GB of pools imported that was 15-45 ms a submission in Kokiri Forest,
// 3-8 fps with 11 ms of GPU work. A pool larger than udmabuf's size limit (/sys/module/udmabuf/parameters/size_limit_mb,
// 64 MB unless raised) is made of several dma-bufs of at most that size, bound one after another into a sparse buffer, so the
// pool is still one buffer with one device address and nobody has to raise the limit. RX_VK_NO_DMABUF=1: host pointers only,
// as before; RX_VK_DMABUF_PIECE_MB=<n>: pieces of at most n MB (to try the sparse path with the limit raised)
struct UdmabufCreate { uint32_t memfd, flags; uint64_t offset, size; };
constexpr unsigned long kUdmabufCreate = (1ul << 30) | (sizeof(UdmabufCreate) << 16) | ('u' << 8) | 0x42;   // (_IOW('u', 0x42, ...))
int dmabuf_memfd = -2, dmabuf_dev = -1, dmabuf_failures = 0;
uint64_t dmabuf_limit = 0;

/// The memory of guest [lo, lo + size) as an imported dma-buf; VK_NULL_HANDLE when udmabuf or the import refused it
VkDeviceMemory dma_buf_memory(uint64_t lo, uint64_t size) {
    UdmabufCreate c{(uint32_t)dmabuf_memfd, 1 /* UDMABUF_FLAGS_CLOEXEC */, lo, size};
    const int fd = ioctl(dmabuf_dev, kUdmabufCreate, &c);
    if (fd < 0) {
        if (dmabuf_failures++ < 8)
            logf("vk: guest %#llx..%#llx: no udmabuf (errno %d)", (unsigned long long)lo, (unsigned long long)(lo + size), errno);
        return VK_NULL_HANDLE;
    }
    static uint32_t type = UINT32_MAX;   // (the same for every dma-buf of the window: asked once)
    if (type == UINT32_MAX) {
        VkMemoryFdPropertiesKHR fp{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
        if (vkGetMemoryFdPropertiesKHR(dev.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fp) == VK_SUCCESS) {
            for (uint32_t i = 0; i < dev.memory.memoryTypeCount && type == UINT32_MAX; i++)   // (host visible, coherent, first)
                if ((fp.memoryTypeBits >> i & 1) &&
                    (dev.memory.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                        (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
                    type = i;
            for (uint32_t i = 0; i < dev.memory.memoryTypeCount && type == UINT32_MAX; i++)
                if (fp.memoryTypeBits >> i & 1) type = i;
        }
        if (type == UINT32_MAX) {
            close(fd);
            if (dmabuf_failures++ < 8) logf("vk: a dma-buf of guest memory has no memory type");
            return VK_NULL_HANDLE;
        }
    }
    VkImportMemoryFdInfoKHR im{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
    im.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    im.fd = fd;
    VkMemoryAllocateFlagsInfo fl{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    fl.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    im.pNext = &fl;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = &im;
    ai.allocationSize = size;
    ai.memoryTypeIndex = type;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    const VkResult r = vkAllocateMemory(dev.device, &ai, nullptr, &memory);
    if (r != VK_SUCCESS) {   // (the fd is the caller's again when the import fails)
        close(fd);
        if (dmabuf_failures++ < 8)
            logf("vk: guest %#llx..%#llx: its dma-buf not imported (VkResult %d)", (unsigned long long)lo,
                 (unsigned long long)(lo + size), (int)r);
        return VK_NULL_HANDLE;
    }
    return memory;
}

/// A pool larger than a dma-buf may be: a sparse buffer, each `piece` bytes of it bound to its own dma-buf
bool import_dma_buf_pieces(uint64_t lo, uint64_t hi, uint64_t piece, Pool& p) {
    VkExternalMemoryBufferCreateInfo eb{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    eb.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.pNext = &eb;
    bi.flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT;
    bi.size = hi - lo;
    bi.usage = kUsage;
    VkBuffer buffer = VK_NULL_HANDLE;
    if (vkCreateBuffer(dev.device, &bi, nullptr, &buffer) != VK_SUCCESS) {
        if (dmabuf_failures++ < 8) logf("vk: pool %#llx..%#llx: no sparse buffer for its dma-bufs", (unsigned long long)lo, (unsigned long long)hi);
        return false;
    }
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(dev.device, buffer, &req);
    // (each piece starts at a multiple of the sparse block size; the last runs to the end of what the buffer needs, past hi
    // when the size rounds up: the window's next pages, which the buffer never reaches)
    const uint64_t align = std::max<uint64_t>(req.alignment, 4096);
    piece = piece / align * align;
    const uint64_t end = lo + req.size;
    if (!piece || (req.size & 4095) || end > host->guest_size()) {
        vkDestroyBuffer(dev.device, buffer, nullptr);
        if (dmabuf_failures++ < 8)
            logf("vk: pool %#llx..%#llx: sparse block %llu, size %llu: not in dma-bufs of %llu", (unsigned long long)lo,
                 (unsigned long long)hi, (unsigned long long)req.alignment, (unsigned long long)req.size, (unsigned long long)piece);
        return false;
    }
    std::vector<VkDeviceMemory> memories;
    std::vector<VkSparseMemoryBind> binds;
    for (uint64_t off = 0; off < req.size; off += piece) {
        const uint64_t n = std::min(piece, req.size - off);
        const VkDeviceMemory m = dma_buf_memory(lo + off, n);
        if (!m) break;
        memories.push_back(m);
        binds.push_back({off, n, m, 0, 0});
    }
    VkResult res = VK_ERROR_UNKNOWN;
    if (binds.size() == (req.size + piece - 1) / piece) {
        VkSparseBufferMemoryBindInfo bb{buffer, (uint32_t)binds.size(), binds.data()};
        VkBindSparseInfo si{VK_STRUCTURE_TYPE_BIND_SPARSE_INFO};
        si.bufferBindCount = 1;
        si.pBufferBinds = &bb;
        static VkFence fence = VK_NULL_HANDLE;   // (the background importer and the recording thread can both import)
        static std::mutex fence_mu;
        std::lock_guard fk(fence_mu);
        if (!fence) {
            VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            vkCreateFence(dev.device, &fi, nullptr, &fence);
        }
        {   // (on the queue, as the sparse textures' binds: nothing uses the new buffer yet, so no wait for earlier work)
            std::lock_guard qk(dev.present_mu);
            queue_flush_locked();
            res = vkQueueBindSparse(dev.queue, 1, &si, fence);
        }
        if (res == VK_SUCCESS) {
            res = vkWaitForFences(dev.device, 1, &fence, VK_TRUE, UINT64_MAX);
            vkResetFences(dev.device, 1, &fence);
        }
    }
    if (res != VK_SUCCESS) {
        vkDestroyBuffer(dev.device, buffer, nullptr);
        for (VkDeviceMemory m : memories) vkFreeMemory(dev.device, m, nullptr);
        if (dmabuf_failures++ < 8)
            logf("vk: pool %#llx..%#llx: its %zu dma-bufs not bound (VkResult %d)", (unsigned long long)lo, (unsigned long long)hi,
                 memories.size(), (int)res);
        return false;
    }
    p.lo = lo, p.hi = hi, p.buffer = buffer, p.memory = memories[0];
    p.pieces.assign(memories.begin() + 1, memories.end());
    return true;
}

bool import_dma_buf(uint64_t lo, uint64_t hi, Pool& p) {
    static uint64_t piece = 0;
    if (dmabuf_memfd == -2) {
        dmabuf_memfd = -1;
        typedef int (*MemfdFn)(void);
        const MemfdFn fn = getenv("RX_VK_NO_DMABUF") ? nullptr : (MemfdFn)dlsym(RTLD_DEFAULT, "hos_posix_guest_memfd");
        const int fd = fn ? fn() : -1;
        if (fd >= 0 && dev.dma_buf_import && vkGetMemoryFdPropertiesKHR) dmabuf_dev = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
        if (fd >= 0 && dmabuf_dev >= 0) {
            dmabuf_memfd = fd;
            dmabuf_limit = 64ull << 20;
            if (FILE* f = fopen("/sys/module/udmabuf/parameters/size_limit_mb", "r")) {
                unsigned long long mb = 0;
                if (fscanf(f, "%llu", &mb) == 1 && mb) dmabuf_limit = mb << 20;
                fclose(f);
            }
            piece = dmabuf_limit;
            if (const char* e = getenv("RX_VK_DMABUF_PIECE_MB"))
                if (atoll(e) > 0) piece = std::min<uint64_t>(piece, (uint64_t)atoll(e) << 20);
        }
        logf("vk: guest memory as dma-bufs: %s", dmabuf_memfd >= 0 ? "yes" : !fn ? "no (no memfd window, or RX_VK_NO_DMABUF)"
                 : fd < 0 ? "no (the window is anonymous memory)" : !dev.dma_buf_import ? "no (VK_EXT_external_memory_dma_buf)"
                 : "no (/dev/udmabuf cannot be opened)");
        if (dmabuf_memfd >= 0)
            logf("vk: dma-bufs of at most %llu MB (udmabuf's limit %llu MB); larger pools %s", (unsigned long long)(piece >> 20),
                 (unsigned long long)(dmabuf_limit >> 20),
                 dev.sparse_buffers ? "are several, in a sparse buffer" : "are host-pointer imports (no sparse binding)");
    }
    if (dmabuf_memfd < 0 || (lo & 4095) || (hi & 4095)) return false;
    if (hi - lo > piece) {
        if (!dev.sparse_buffers || !import_dma_buf_pieces(lo, hi, piece, p)) return false;
    } else {
        VkExternalMemoryBufferCreateInfo eb{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
        eb.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.pNext = &eb;
        bi.size = hi - lo;
        bi.usage = kUsage;
        VkBuffer buffer = VK_NULL_HANDLE;
        if (vkCreateBuffer(dev.device, &bi, nullptr, &buffer) != VK_SUCCESS) return false;
        const VkDeviceMemory memory = dma_buf_memory(lo, hi - lo);
        if (!memory) {
            vkDestroyBuffer(dev.device, buffer, nullptr);
            return false;
        }
        vkBindBufferMemory(dev.device, buffer, memory, 0);
        p.lo = lo, p.hi = hi, p.buffer = buffer, p.memory = memory;
    }
    VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    da.buffer = p.buffer;
    p.address = vkGetBufferDeviceAddress(dev.device, &da);
    if (getenv("RX_VK_LOG_ADDR"))   // (as import_pool's: which buffer a device fault's address is in)
        logf("vk: addr dma-buf %#llx..%#llx at gpu %#llx..%#llx", (unsigned long long)lo, (unsigned long long)hi,
             (unsigned long long)p.address, (unsigned long long)(p.address + (hi - lo)));
    static int64_t n = 0, bytes = 0, sparse = 0;
    n++, bytes += (int64_t)(hi - lo), sparse += !p.pieces.empty();
    if (n <= 4 || n % 64 == 0 || (!p.pieces.empty() && sparse <= 4))
        logf("vk: pool %#llx..%#llx a dma-buf%s (%lld so, %lld in pieces, %lld MB)", (unsigned long long)lo, (unsigned long long)hi,
             p.pieces.empty() ? "" : "s in a sparse buffer", (long long)n, (long long)sparse, (long long)(bytes >> 20));
    return true;
}
#endif

bool import_pool(uint64_t lo, uint64_t hi, Pool& p) {
    import_cooling = false;
    import_result = VK_SUCCESS;
    if (import_on_hold(lo, hi)) return false;   // (tried recently and failed: not again until the hold ends)
    if (hi <= lo || hi - lo > max_import_size) {
        logf("vk: import %#llx..%#llx exceeds the device's allocation/buffer limit", (unsigned long long)lo,
             (unsigned long long)hi);
        import_remember_failure(lo, hi);
        return false;
    }
    p.lo = lo;
    p.hi = hi;
    uint8_t* ptr = host->guest_base() + lo;
    const uint64_t bytes = hi - lo;
    using clk = std::chrono::steady_clock;
    const auto t0 = clk::now();
    clk::time_point t1, t2, t3, t4;
    struct Report {   // (where an import's time goes: logged for slow ones, 12 times)
        const clk::time_point &a, &b, &c, &d, &e;
        ~Report() {
            if (e.time_since_epoch().count() == 0) return;
            const double total = std::chrono::duration<double, std::milli>(e - a).count();
            static int logged;
            if (total < 2.0 || logged++ >= 12) return;
            auto ms = [](clk::time_point x, clk::time_point y) { return std::chrono::duration<double, std::milli>(y - x).count(); };
            logf("vk: import %.1f ms: commit+properties %.1f, buffer %.1f, allocate %.1f, bind+address %.1f", total, ms(a, b),
                 ms(b, c), ms(c, d), ms(d, e));
        }
    } report{t0, t1, t2, t3, t4};
    // (pages the game has not touched yet may be only reserved: committed now, as the pool's memory is usable)
    // (hookshot) One commit avoids the slow query walk, but resets protection on committed pages too. Keep the walk
    // for CPU write watches: commit only reserved pages, leaving the watched pages read-only.
    // ponytail: add new watch switches here; query the host if watches stop requiring a startup switch.
    static const bool page_watches = getenv("RX_NVN_WATCH_TEX") || getenv("RX_NVN_WATCH_TEXELS_SRC") ||
                                     getenv("RX_VK_WATCH_CPU") || getenv("RX_VK_VERIFY_DIM");
    if (page_watches || VirtualAlloc(ptr, (SIZE_T)bytes, MEM_COMMIT, PAGE_READWRITE) != ptr)
        for (uint8_t* q = ptr; q < ptr + bytes;) {
            MEMORY_BASIC_INFORMATION mi{};
            if (VirtualQuery(q, &mi, sizeof mi) != sizeof mi) break;
            uint8_t* end = std::min((uint8_t*)mi.BaseAddress + mi.RegionSize, ptr + bytes);
            if (mi.State == MEM_RESERVE) VirtualAlloc(q, (size_t)(end - q), MEM_COMMIT, PAGE_READWRITE);
            q = end;
        }
#ifdef __linux__
    if (import_dma_buf(lo, hi, p)) {
        t1 = t2 = t3 = t4 = clk::now();
        return true;
    }
#endif
    // (the host allocations' memory type: the same for every pointer, asked once)
    static uint32_t type = UINT32_MAX;
    if (type == UINT32_MAX) {
        VkMemoryHostPointerPropertiesEXT hp{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
        if (vkGetMemoryHostPointerPropertiesEXT(dev.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, ptr,
                                                &hp) != VK_SUCCESS) {
            import_remember_failure(lo, hi);
            return false;
        }
        for (uint32_t i = 0; i < dev.memory.memoryTypeCount && type == UINT32_MAX; i++)   // (host visible, coherent)
            if ((hp.memoryTypeBits >> i & 1) &&
                (dev.memory.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
                type = i;
        if (type == UINT32_MAX) {
            import_remember_failure(lo, hi);
            return false;
        }
    }
    t1 = clk::now();
    VkExternalMemoryBufferCreateInfo eb{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    eb.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.pNext = &eb;
    bi.size = bytes;
    bi.usage = kUsage;
    if (vkCreateBuffer(dev.device, &bi, nullptr, &p.buffer) != VK_SUCCESS) {
        import_remember_failure(lo, hi);
        return false;
    }
    t2 = clk::now();
    VkImportMemoryHostPointerInfoEXT im{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
    im.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    im.pHostPointer = ptr;
    VkMemoryAllocateFlagsInfo fl{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    fl.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    im.pNext = &fl;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = &im;
    ai.allocationSize = bytes;
    ai.memoryTypeIndex = type;
    const VkResult allocated = vkAllocateMemory(dev.device, &ai, nullptr, &p.memory);
    if (allocated != VK_SUCCESS) {
        import_result = allocated;
        vkDestroyBuffer(dev.device, p.buffer, nullptr);
        p.buffer = VK_NULL_HANDLE;
        import_remember_failure(lo, hi);
        return false;
    }
    t3 = clk::now();
    vkBindBufferMemory(dev.device, p.buffer, p.memory, 0);
    VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    da.buffer = p.buffer;
    p.address = vkGetBufferDeviceAddress(dev.device, &da);
    if (getenv("RX_VK_LOG_ADDR"))   // (diagnostics: which buffer a device fault's address is in)
        logf("vk: addr import %#llx..%#llx at gpu %#llx..%#llx", (unsigned long long)lo, (unsigned long long)hi,
             (unsigned long long)p.address, (unsigned long long)(p.address + (hi - lo)));
    t4 = clk::now();
    return true;
}

} // namespace

// ---- (Triforce, 2026-11-11) The memory handling played on NVIDIA cards only, on trial on the others ----
// Mirrors in pieces (below) and the textures' compaction (vk_texture.cpp compact_on) were measured on NVIDIA cards alone, and
// were off elsewhere unless a line in triforce_env.txt asked for them: nobody with an AMD or Intel card got them. Now any card
// has them, and a card of another make has them on trial: triforce_memtrial.txt beside the executable says how that went.
//   trying: a session has them on. After kTrialSeconds of play (5 minutes) the file says ok, for good.
//   A device lost while trying: off, at once. An unhandled exception, or a session that ended with no word (killed while
//   hung, the PC switched off): a failure; at the second, off. Closing the window is no failure: the file says new again.
//   off: they stay off on this PC with this driver (the file names it: a new driver starts over; so does deleting the file).
// RX_VK_MIRROR_PIECES and RX_VK_COMPACT, 0 or 1, still decide either one whatever the file says.
// (lab: RX_VK_TEST_VENDOR=<hex> plays another make's card; RX_VK_TEST_TRIAL_S=<n>: the trial's length)
namespace memtrial {
enum State { NEW, TRYING, OK, OFF };
const char* const kNames[] = {"new", "trying", "ok", "off"};
constexpr int kVersion = 1;
State state = NEW;
int fails = 0;
bool active = false;                 // (this session has them on, on trial)
std::atomic<bool> crashed{false};    // (an unhandled exception or a device lost: the exit handler leaves the file alone)
std::mutex mu;
std::chrono::steady_clock::time_point began;
LPTOP_LEVEL_EXCEPTION_FILTER prev_filter = nullptr;
std::string path() {
    char exe[MAX_PATH];
    const DWORD n = game_module_path(exe, MAX_PATH);
    std::string dir(exe, n);
    return dir.substr(0, dir.find_last_of("\\/") + 1) + "triforce_memtrial.txt";
}
void write(State s, int f, const char* why) {   // (no logging here: called from the exception filter and the exit handler)
    if (FILE* out = fopen(path().c_str(), "w")) {
        fprintf(out, "v=%d vendor=%x device=%x driver=%x state=%s fails=%d\n# %s\n# Triforce's trial of its newer memory handling on "
                     "this card. Delete this file to start the trial again.\n", kVersion, (unsigned)dev.props.vendorID,
                (unsigned)dev.props.deviceID, (unsigned)dev.props.driverVersion, kNames[s], f, why);
        fclose(out);
    }
}
LONG WINAPI filter(EXCEPTION_POINTERS* info) {
    if (active && !crashed.exchange(true)) {
        const int f = fails + 1;
        write(f >= 2 ? OFF : TRYING, f, f >= 2 ? "two sessions ended in a crash with it on: off" : "a session ended in a crash with it on");
    }
    return prev_filter ? prev_filter(info) : EXCEPTION_CONTINUE_SEARCH;
}
void at_exit() {   // (the window closed, or any orderly exit: no verdict, the next session tries again)
    if (active && !crashed.load() && state == TRYING) write(NEW, fails, "the last session ended before the trial was over");
}
uint32_t vendor() {
    static const uint32_t v = getenv("RX_VK_TEST_VENDOR") ? (uint32_t)strtoul(getenv("RX_VK_TEST_VENDOR"), nullptr, 16) : dev.props.vendorID;
    return v;
}
}   // namespace memtrial
bool memory_trial_on() {
    using namespace memtrial;
    static const bool on = [] {
        if (vendor() == 0x10DE) return true;   // (played there)
        std::lock_guard lk(mu);
        int v = 0, f = 0;
        unsigned ven = 0, device = 0, driver = 0;
        char name[16] = "";
        State was = NEW;
        if (FILE* in = fopen(path().c_str(), "r")) {
            if (fscanf(in, "v=%d vendor=%x device=%x driver=%x state=%15s fails=%d", &v, &ven, &device, &driver, name, &f) == 6 &&
                v == kVersion && ven == dev.props.vendorID && device == dev.props.deviceID && driver == dev.props.driverVersion)
                for (int i = 0; i < 4; i++)
                    if (!strcmp(name, kNames[i])) was = (State)i, fails = f;
            fclose(in);
        }
        if (was == TRYING) fails++;   // (a session that left no word: killed, hung, or the PC went off)
        if (was == OFF || fails >= 2) {
            state = OFF;
            if (was != OFF) write(OFF, fails, "two sessions with it on ended in a crash or without a word: off");
            logf("vk: the newer memory handling (mirrors in pieces, compacted textures) is off on this card: %s (delete "
                 "triforce_memtrial.txt to try it again)", was == OFF ? "an earlier session lost the GPU or crashed with it on" :
                 "two sessions with it on ended in a crash or without a word");
            return false;
        }
        if (was == OK) {
            state = OK;
            logf("vk: the newer memory handling (mirrors in pieces, compacted textures) is on: its trial on this card passed");
            return true;
        }
        state = TRYING, active = true, began = std::chrono::steady_clock::now();
        write(TRYING, fails, "a session has it on");
        prev_filter = SetUnhandledExceptionFilter(filter);
        atexit(at_exit);
        logf("vk: the newer memory handling (mirrors in pieces, compacted textures) is on trial on this card (%d earlier session(s) "
             "ended badly); triforce_memtrial.txt keeps the result", fails);
        return true;
    }();
    return on;
}
/// (texture_frame, every 600 presents) the trial passed once the game has played kTrialSeconds with it on
void memory_trial_tick() {
    using namespace memtrial;
    if (!active || state != TRYING) return;
    static const int64_t seconds = getenv("RX_VK_TEST_TRIAL_S") ? atoll(getenv("RX_VK_TEST_TRIAL_S")) : 300;
    if (std::chrono::steady_clock::now() - began < std::chrono::seconds(seconds)) return;
    std::lock_guard lk(mu);
    state = OK;
    write(OK, 0, "played with it on: kept");
    logf("vk: the newer memory handling passed its trial on this card: kept on from now on");
}
/// (device_lost) the GPU was lost with the trial's handling on: off from the next start
void memory_trial_device_lost() {
    using namespace memtrial;
    if (vendor() == 0x10DE) return;
    if (state == OK) {   // (after a trial that passed: one more trial, and off if that one ends badly too)
        write(NEW, 1, "the GPU device was lost in a session after the trial had passed: tried once more");
        return;
    }
    if (!active || crashed.exchange(true)) return;
    write(OFF, fails + 1, "the GPU device was lost with it on: off");
}

// (hookshot) guest memory's page table, for the pointers shaders follow (spirv_emit_context.cpp): a page's device address, 0 for none. Written as
// pools come and go, before the submissions that can reach them
uint64_t* guest_pages = nullptr;
uint64_t guest_pages_address = 0;
VkBuffer guest_pages_buffer = VK_NULL_HANDLE;
// (hookshot, 11-07) A change to the table made while a submission is recorded goes into that submission's upload commands, after
// the mirror copies it records there, instead of into the table at once: the table is read by the GPU, and work submitted
// before (still running, or queued) read the new entries early. A pool's range zeroed at its merge read as unmapped (zero
// counts: the culling's foliage gone for a frame), and a mirror made but not yet filled read as garbage: the foliage
// culling 53caac5f looped on a count from it until Windows reset the driver (the device lost of ~1 start in 5, always
// after pool merges, 2 s after the GPU stopped)
struct TableRun { uint64_t first, count, address; };
thread_local std::vector<TableRun> table_pending;
thread_local bool table_defer = false;
void guest_pages_set(uint64_t lo, uint64_t hi, uint64_t address) {
    if (!guest_pages) return;
    // (on by default since 11-08: 14 of 14 starts without the device lost, against ~1 in 7 written at once; one-frame flashes
    // 0-1 bright / 4 dark in two 40 s Kokiri runs, as many as without it, and the same fps. One earlier run with 21 dark ones was
    // the runs' spread, not this. RX_VK_TABLE_NOW=1: written at once, as before)
    static const bool now = getenv("RX_VK_TABLE_NOW") != nullptr;
    // (two windows, as spirv_emit_context.cpp reads them: 8 GB from 0x1000000000, then 4 GB from 0x7f00000000)
    const struct { uint64_t base, bytes, first; } windows[] = {{0x1000000000ull, 8ull << 30, 0}, {0x7f00000000ull, 4ull << 30, (8ull << 30) >> 12}};
    for (const auto& w : windows) {
        const uint64_t a0 = std::max(lo, w.base) & ~0xFFFull, a1 = std::min(hi, w.base + w.bytes);
        if (a0 >= a1) continue;
        const uint64_t first = w.first + ((a0 - w.base) >> 12), count = (a1 - a0 + 0xFFF) >> 12;
        const uint64_t at = address ? address + (a0 - lo) : 0;
        if (table_defer && !now) {
            table_pending.push_back({first, count, at});
            continue;
        }
        for (uint64_t i = 0; i < count; i++) guest_pages[first + i] = at ? at + (i << 12) : 0;
    }
}
void memory_table_begin() {
    if (!table_pending.empty()) {   // (left by a recording that never ended: written now)
        table_defer = false;
        for (const TableRun& r : table_pending)
            for (uint64_t i = 0; i < r.count; i++) guest_pages[r.first + i] = r.address ? r.address + (i << 12) : 0;
        table_pending.clear();
    }
    table_defer = guest_pages != nullptr;
}
// (at the end of the recording's uploads: the changes, the last one for each page, as transfers into the table; the
// upload command buffer's barriers order them after the work submitted before and before this submission's)
bool memory_table_flush() {
    table_defer = false;
    if (table_pending.empty()) return false;
    std::vector<std::pair<uint64_t, uint64_t>> pages;   // (page, value), in the order they were set
    for (const TableRun& r : table_pending)
        for (uint64_t i = 0; i < r.count; i++) pages.push_back({r.first + i, r.address ? r.address + (i << 12) : 0});
    table_pending.clear();
    std::stable_sort(pages.begin(), pages.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<uint64_t> run;
    VkCommandBuffer cmd = upload_command_buffer();
    GprofSpan gp(cmd, "ptable update (guest page table)");
    gp.items = 0;
    uint64_t run_first = 0;
    const auto emit = [&] {
        if (!run.empty()) vkCmdUpdateBuffer(cmd, guest_pages_buffer, run_first * 8, run.size() * 8, run.data()), gp.items++, gp.bytes += run.size() * 8;
        run.clear();
    };
    for (size_t i = 0; i < pages.size(); i++) {
        if (i + 1 < pages.size() && pages[i + 1].first == pages[i].first) continue;   // (a later change of the same page wins)
        if (!run.empty() && (pages[i].first != run_first + run.size() || run.size() == 8192)) emit();
        if (run.empty()) run_first = pages[i].first;
        run.push_back(pages[i].second);
    }
    emit();
    return true;
}

uint64_t memory_guest_pages() {
    static std::mutex mu;
    std::lock_guard lk(mu);
    static bool tried = false;
    if (tried) return guest_pages_address;
    tried = true;
    if (getenv("RX_VK_NO_FOLLOW")) return 0;   // (on: Kokiri's lighting. The menus' blank text was the rule in graphics stages, now compute only)
    VkBuffer b = VK_NULL_HANDLE;
    VmaAllocation a = nullptr;
    void* mapped = nullptr;
    const uint64_t bytes = ((8ull << 30 >> 12) + (4ull << 30 >> 12)) * 8;
    if (!create_buffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                       true, b, a, &mapped, true) || !mapped) {
        logf("vk: no page table of guest memory: pointers in no bound buffer read as 0");
        return 0;
    }
    std::memset(mapped, 0, bytes);
    VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    da.buffer = b;
    guest_pages_address = vkGetBufferDeviceAddress(dev.device, &da);
    guest_pages_buffer = b;
    std::lock_guard pk(pools_mu);
    guest_pages = (uint64_t*)mapped;
    for (auto& kv : imported) guest_pages_set(kv.second.lo, kv.second.hi, kv.second.address);
    logf("vk: page table of guest memory at %#llx (%zu pools)", (unsigned long long)guest_pages_address, imported.size());
    return guest_pages_address;
}

bool memory_init() {
    if (!dev.external_memory_host) {
        logf("vk: no VK_EXT_external_memory_host: the guest memory cannot be imported (needed for now)");
        return false;
    }
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    VkPhysicalDeviceVulkan11Properties p11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
    VkPhysicalDeviceVulkan13Properties p13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
    hp.pNext = &p11;
    p11.pNext = &p13;
    p2.pNext = &hp;
    vkGetPhysicalDeviceProperties2(dev.physical, &p2);
    align = std::max<uint64_t>(hp.minImportedHostPointerAlignment, 4096);
    max_import_size = std::min(p11.maxMemoryAllocationSize, p13.maxBufferSize);
    logf("vk: guest memory imported in place (alignment %llu)", (unsigned long long)align);
    return true;
}

/// Refresh the guest's pool table (no Vulkan work: pools are imported as they are used)
void memory_sync() {
    static std::mutex sync_mu;   // (called from every thread that touches memory)
    std::lock_guard<std::mutex> sync_lk(sync_mu);
    const uint32_t g = host->maps_generation();
    if (g == generation) return;
    generation = g;
    static thread_local std::vector<std::array<uint64_t, 2>> list(8192);
    const int n = host->gpu_pools(reinterpret_cast<uint64_t(*)[2]>(list.data()), (int)list.size());
    std::vector<GuestPool> next;
    next.reserve(n > 0 ? (size_t)n : 0);
    for (int i = 0; i < n; i++) {
        if (!list[i][1]) continue;
        GuestPool p;
        p.lo = list[i][0] & ~(align - 1);
        p.hi = (list[i][0] + list[i][1] + align - 1) & ~(align - 1);
        next.push_back(p);
    }
    std::sort(next.begin(), next.end(), [](const GuestPool& a, const GuestPool& b) { return a.lo < b.lo; });
    std::lock_guard lk(pools_mu);
    guest_pools.swap(next);
    guest_pools_gen.fetch_add(1, std::memory_order_relaxed);
    last_pool = nullptr;
    std::fill(std::begin(pool_cache), std::end(pool_cache), nullptr);
    static int logged;
    if (logged++ < 8) logf("vk: %zu pools mapped", guest_pools.size());
}

// ---- page write generations ----
// Two levels over the guest's 512 GB (2^27 pages of 4 KB): chunks of 64K page generations made when a page of the
// chunk is first stamped, and the newest generation of each 2 MB region (512 pages), which lets a check skip a big
// texture's clean regions without looking at their pages. 0: never written.
namespace {
constexpr uint64_t kPageShift = 12, kChunkPages = 1u << 16, kRegionPages = 512;
constexpr uint64_t kPages = 1ull << 27;
std::vector<std::unique_ptr<uint32_t[]>> gen_chunks(kPages / kChunkPages);
std::vector<uint32_t> region_gen(kPages / kRegionPages);
// (and the generation every page of a region was written at, at least: a large stamp (a storage buffer a dispatch may
// write, hundreds of MB) marks whole regions, not 512 pages each; it took dispatches 8-150 ms)
std::vector<uint32_t> region_full(kPages / kRegionPages);
inline uint32_t load_u32(uint32_t& x) {
    return std::atomic_ref<uint32_t>(x).load(std::memory_order_acquire);
}
inline void store_u32(uint32_t& x, uint32_t v) {
    std::atomic_ref<uint32_t>(x).store(v, std::memory_order_release);
}
inline uint32_t page_gen(uint64_t p) {
    const auto& c = gen_chunks[p / kChunkPages];
    return std::max(c ? load_u32(c[p % kChunkPages]) : 0u, load_u32(region_full[p / kRegionPages]));
}
uint32_t gen_now = 1;
uint64_t pages_collected = 0;
// (Triforce, RX_VK_STAMP_SRC=1, a diagnostic: who made each write generation. 1: the write watch's collection, 2: a capture's
// take, else the caller of memory_stamp. A texture uploaded again asks by its dirty page: memory_stamp_who)
bool stamp_src_on() {
    static const bool v = getenv("RX_VK_STAMP_SRC") != nullptr;
    return v;
}
std::mutex stamp_src_mu;
std::vector<uintptr_t> stamp_src;   // (by generation)
thread_local uintptr_t tl_stamp_who;
void stamp_src_note(uint32_t g, uintptr_t who) {
    if (!stamp_src_on()) return;
    std::lock_guard lk(stamp_src_mu);
    if (stamp_src.size() <= g) stamp_src.resize((size_t)g + 65536);
    stamp_src[g] = who;
}

void stamp_page(uint64_t p, uint32_t g) {
    if (p >= kPages) return;
    auto& c = gen_chunks[p / kChunkPages];
    if (!c) {
        c.reset(new uint32_t[kChunkPages]);
        std::fill(c.get(), c.get() + kChunkPages, 0u);
    }
    store_u32(c[p % kChunkPages], g);
    region_gen[p / kRegionPages] = g;
}
// (Triforce) A capture's stamp of a texture storage page that held the bytes of the page's last copy (take_capture): two words a
// page, the stamp and the generation the page had before the run of such stamps. While the page's generation is still that stamp,
// a texture asking what was written since its upload is told the generation before (page_gen_tex); any other stamp ends it
std::vector<std::unique_ptr<uint32_t[]>> same_chunks(kPages / kChunkPages);
inline uint32_t page_gen_tex(uint64_t p) {
    const uint32_t g = page_gen(p);
    const auto& c = same_chunks[p / kChunkPages];
    if (!c || !g) return g;
    const uint32_t s = load_u32(c[(p % kChunkPages) * 2]);
    return s == g ? load_u32(c[(p % kChunkPages) * 2 + 1]) : g;
}
void page_same_set(uint64_t p, uint32_t stamp, uint32_t before) {
    if (p >= kPages || !stamp) return;
    auto& c = same_chunks[p / kChunkPages];
    if (!c) {
        c.reset(new uint32_t[kChunkPages * 2]);
        std::fill(c.get(), c.get() + kChunkPages * 2, 0u);
    }
    store_u32(c[(p % kChunkPages) * 2 + 1], before);
    store_u32(c[(p % kChunkPages) * 2], stamp);
}
} // namespace

namespace { void cap_view_bump(); }   // (defined with cap_view_gen below, in this file's anonymous namespace)
const std::vector<std::pair<uint64_t, uint64_t>>& capture_ranges_merged();
void shadow_written(const uint32_t* pages, int64_t n);   // (the write-back's diff, below: pages the captures took)
void shadow_written_collected(const uint32_t* pages, int64_t n);   // (the write-back's diff, below: pages the collection found)
void memory_collect_written() {
    uint32_t* pages = nullptr;
    const int64_t n = host->collect_written(&pages);
    if (n > 0) {
        const uint32_t g = ++gen_now;
        stamp_src_note(g, 1);
        // (the captured ranges once a call: a lock, a pool-list refresh and three searches for every dirty page cost the
        // replay thousands of lock cycles a submit, the cost that retired gpu_pages' lock below)
        const auto& owned = capture_ranges_merged();
        const auto in_capture = [&owned](uint64_t guest) {
            auto it = std::upper_bound(owned.begin(), owned.end(), std::make_pair(guest, ~uint64_t(0)));
            return it != owned.begin() && guest < std::prev(it)->second;
        };
        for (int64_t i = 0; i < n; i++) {
            const uint64_t guest = (uint64_t)pages[i] << kPageShift;
            if (!owned.empty() && in_capture(guest)) {   // (written after its submit's capture: the next capture takes it, as it is then)
                uint64_t avail = 0;
                if (const uint8_t* q = host->gpu_ptr(guest, &avail)) host->mark_written(q, 4096);
                continue;
            }
            stamp_page(pages[i], g);
        }
        pages_collected += (uint64_t)n;
        cap_view_bump();
        // (hookshot) every page the CPU wrote, captured later or not: unknown to the write-back diff's shadows before this
        // recording's write-back. With RX_VK_CAPTURE=0 this is the only report of the game's writes (capture_make returns
        // first); with captures, it is the pages written after this submission's capture (PR 1109's review)
        shadow_written_collected(pages, n);
    }
    if (pages) host->free_pages(pages);
}
uint32_t memory_generation() {
    return gen_now;
}
/// (RX_VK_STAMP_SRC) Who wrote the page at guest last (stamp_src above), its generation, and memory_stamp's own address to place it by
uintptr_t memory_stamp_who(uint64_t guest, uint32_t* gen, uintptr_t* anchor) {
    const uint32_t g = page_gen(guest >> kPageShift);
    *gen = g, *anchor = (uintptr_t)&memory_stamp;
    std::lock_guard lk(stamp_src_mu);
    return g < stamp_src.size() ? stamp_src[g] : 0;
}
bool memory_written_since(uint64_t guest, uint64_t size, uint32_t gen) {
    if (!size) return false;
    const uint64_t p0 = guest >> kPageShift, p1 = std::min((guest + size - 1) >> kPageShift, kPages - 1);
    for (uint64_t r = p0 / kRegionPages; r <= p1 / kRegionPages; r++) {
        if (region_gen[r] <= gen) continue;
        if (load_u32(region_full[r]) > gen) return true;
        const uint64_t a = std::max(p0, r * kRegionPages), b = std::min(p1, r * kRegionPages + kRegionPages - 1);
        for (uint64_t p = a; p <= b; p++)
            if (page_gen(p) > gen) return true;
    }
    return false;
}
void memory_written_runs(uint64_t guest, uint64_t size, uint32_t gen, std::vector<std::pair<uint64_t, uint64_t>>& out, bool tex) {
    if (!size) return;
    const uint64_t p0 = guest >> kPageShift, p1 = std::min((guest + size - 1) >> kPageShift, kPages - 1);
    for (uint64_t r = p0 / kRegionPages; r <= p1 / kRegionPages; r++) {
        if (region_gen[r] <= gen) continue;
        const uint64_t a = std::max(p0, r * kRegionPages), b = std::min(p1, r * kRegionPages + kRegionPages - 1);
        for (uint64_t p = a; p <= b; p++) {
            // (tex, Triforce: a texture's question: a capture's stamp of the same bytes as before is not a write, page_gen_tex)
            if ((tex ? page_gen_tex(p) : page_gen(p)) <= gen) continue;
            const uint64_t lo = std::max(p << kPageShift, guest) - guest;
            const uint64_t hi = std::min((p + 1) << kPageShift, guest + size) - guest;
            if (!out.empty() && out.back().second == lo) out.back().second = hi;
            else out.push_back({lo, hi});
        }
    }
}
void memory_drop_shadow(uint64_t guest, uint64_t size);
// (a GPU write: RX_VK_GPU_PAGES=1 notes the pages, and the mirror syncs that copy the CPU's version of one over the
// mirror are counted (the GPU's bytes in that page replaced by what guest memory or a capture held: a page the CPU
// and the GPU both write). ~1M a race, no effect seen; off by default: its lock on every dirty page cost the replay)
std::mutex gpu_pages_mu;
std::unordered_map<uint32_t, uint32_t> gpu_pages;   // page -> the generation of the GPU's last write
int64_t st_sync_gpu_pages, st_sync_gpu_live;
bool gpu_pages_on() {
    static const bool v = getenv("RX_VK_GPU_PAGES") != nullptr;
    return v;
}
void memory_stamp_gpu(uint64_t guest, uint64_t size) {
    if (!size) return;
    tl_stamp_who = (uintptr_t)__builtin_return_address(0);
    memory_stamp(guest, size);
    tl_stamp_who = 0;
    if (!gpu_pages_on()) return;
    std::lock_guard lk(gpu_pages_mu);
    const uint64_t p0 = guest >> kPageShift, p1 = (guest + size - 1) >> kPageShift;
    if (p1 - p0 > 4096) return;   // (big copies: not tracked)
    for (uint64_t p = p0; p <= p1; p++) gpu_pages[(uint32_t)p] = gen_now;
    if (gpu_pages.size() > 2000000) gpu_pages.clear();
}
bool gpu_page(uint64_t page) {
    if (!gpu_pages_on()) return false;
    std::lock_guard lk(gpu_pages_mu);
    return gpu_pages.count((uint32_t)page) != 0;
}

// (RX_VK_PROTECT_BARE=1, an experiment for the one-frame foliage flicker and the floating trees) The ranges a shader writes
// through a table pointer (vk_draw.cpp: a bare entry, only its first 64 bytes marked written) are GPU-written pages the
// mirror syncs know nothing of: a CPU write anywhere on such a page copied the CPU's whole page over the GPU's records
// (NX2navi found the same: "trees drawn from records lost that way stretched into columns reaching the sky", and culled
// foliage drew garbage for a frame). Pages noted here keep the GPU's version when the CPU also wrote them since the
// pool's last sync. Nothing is copied back to guest memory (that overwrote the CPU's own data beside them once).
std::mutex prot_mu;
std::unordered_map<uint32_t, uint32_t> prot_pages;   // page -> gen_now when a shader last wrote through it
std::atomic<int64_t> st_prot_kept{0};
bool protect_bare_on() {
    static const bool v = getenv("RX_VK_PROTECT_BARE") && getenv("RX_VK_PROTECT_BARE")[0] == '1';
    return v;
}
bool prot_page_since(uint64_t page, uint32_t synced) {
    std::lock_guard lk(prot_mu);
    const auto it = prot_pages.find((uint32_t)page);
    return it != prot_pages.end() && it->second >= synced;
}
void memory_protect_gpu(uint64_t guest, uint64_t size) {
    if (!size || !protect_bare_on()) return;
    const uint64_t p0 = guest >> kPageShift, p1 = (guest + size - 1) >> kPageShift;
    if (p1 - p0 > 16384) return;   // (64 MB at most: a guess past that is not the GPU's records)
    std::lock_guard lk(prot_mu);
    for (uint64_t p = p0; p <= p1; p++) prot_pages[(uint32_t)p] = gen_now;
    if (prot_pages.size() > 4000000) prot_pages.clear();
}

void memory_stamp(uint64_t guest, uint64_t size) {
    if (!size) return;
    {   // (RX_VK_LOG_STAMP=<lo>:<hi> (hex guest addresses): the stamps there, 300 lines, with their callers)
        static const std::pair<uint64_t, uint64_t> range = [] {
            unsigned long long a = 0, b = 0;
            if (const char* e = getenv("RX_VK_LOG_STAMP")) sscanf(e, "%llx:%llx", &a, &b);
            return std::pair<uint64_t, uint64_t>{a, b};
        }();
        static int logged;
        if (range.second && guest < range.second && guest + size > range.first && logged++ < 300)
            logf("vk: stamp %#llx +%#llx (gen %u) from %p", (unsigned long long)guest, (unsigned long long)size, gen_now + 1,
                 __builtin_return_address(0));
    }
    memory_drop_shadow(guest, size);   // (a write the capture did not take: guest memory is newer than its copy)
    const uint32_t g = ++gen_now;
    stamp_src_note(g, tl_stamp_who ? tl_stamp_who : (uintptr_t)__builtin_return_address(0));
    const uint64_t p0 = guest >> kPageShift, p1 = std::min((guest + size - 1) >> kPageShift, kPages - 1);
    for (uint64_t p = p0; p <= p1;) {
        if (p % kRegionPages == 0 && p + kRegionPages - 1 <= p1) {   // (a whole region)
            store_u32(region_full[p / kRegionPages], g);
            region_gen[p / kRegionPages] = g;
            p += kRegionPages;
            continue;
        }
        stamp_page(p++, g);
    }
    cap_view_bump();   // (a replay read's cached page is this generation's answer, not the one from before the write)
}
uint64_t memory_pages_collected() {
    return pages_collected;
}

// ---- submit-time capture ----
// MKW writes some of a frame's data in place right after its submit (the console's GPU has read it by then). The
// replay records a submit later, and the mirrors were filled from guest memory as it was then: a frame's terrain
// tiles read the next one's constants and flashed as walls. The pages are now copied as they are at the submit.
namespace {
struct Capture {
    std::vector<uint32_t> pages;     // (written since the last submit)
    std::vector<uint32_t> copied;    // (those copied, in order)
    std::vector<uint32_t> slots;     // (and where each is in the buffer: 4 KB a slot, the page's index in pages)
    // (copied by the submitting thread straight into the buffer the GPU copies the mirrors' pages from, in cached host
    // memory: the replay's own copy of them into one was ~0.6 ms a submit once the game's small pools were captured)
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    void* map = nullptr;
    uint64_t bsize = 0;
    size_t nbytes = 0;
    std::vector<uint32_t> late;      // (indices into pages: copied again by the replay once late_value is done)
    uint64_t late_value = 0;
};
std::mutex cap_mu;
std::deque<std::unique_ptr<Capture>> captures;
std::vector<std::unique_ptr<Capture>> spare;   // (taken ones, their buffers kept)
// ---- the captures' pages, for the mirror syncs ----
// A capture takes the pages written since the last submit; a pool its submission does not bind is synced at a later
// one, whose capture does not have those pages: copied from guest memory as it was at that replay, by when MKW had
// rewritten them for a later frame (it writes several submits ahead of the replay), a frame's objects drew with
// another one's transforms and instance data (the one-frame flashes). Each page a capture took is in a table by page:
// the capture (the generation its take stamped the pages with) and the page's slot in that capture's buffer. The
// entry holds while the page's generation is still that one (a later capture re-takes the page, a write the captures
// did not take (memory_stamp) makes guest memory newer) and the capture's buffer is kept (the last kKeep captures, and
// until the work that copies from it is done); a sync copies a dirty page from there, else from guest memory.
// (A flat table and generations: the hash maps' node allocations took ~0.7 ms a submit, the replay's.)
// (RX_VK_SYNC_LIVE=1: dirty pages not in their submission's capture read from guest memory, as before)
struct CapEntry { uint32_t gen, slot; };
constexpr uint64_t kCapChunkPages = 1u << 16;   // (a chunk of the table: 256 MB of guest memory, made when first used)
std::vector<std::unique_ptr<CapEntry[]>> cap_table;
inline CapEntry cap_entry(uint64_t page) {
    const uint64_t c = page / kCapChunkPages;
    if (c >= cap_table.size() || !cap_table[c]) return {0, 0};
    return cap_table[c][page % kCapChunkPages];
}
inline void cap_entry_set(uint64_t page, CapEntry e) {
    const uint64_t c = page / kCapChunkPages;
    if (c >= cap_table.size()) cap_table.resize(c + 1);
    if (!cap_table[c]) {
        cap_table[c].reset(new CapEntry[kCapChunkPages]);
        std::memset(cap_table[c].get(), 0, kCapChunkPages * sizeof(CapEntry));
    }
    cap_table[c][page % kCapChunkPages] = e;
}
// (each kept capture's copy on the GPU, by its generation)
struct CapBuf { uint64_t value; VkBuffer buffer; VmaAllocation alloc; uint64_t size; void* map; uint64_t last_use = 0; };
std::map<uint32_t, CapBuf> cap_bufs;
uint64_t cap_bytes = 0;
constexpr size_t kKeep = 24;
constexpr uint64_t kCapBytesMax = 768ull << 20;   // (and no more than this in them: loads capture hundreds of MB)
// (retired buffers, for the next captures: making and freeing a 5 MB buffer every submit cost the replay too)
struct FreeBuf { VkBuffer buffer; VmaAllocation alloc; void* map; uint64_t size; };
std::vector<FreeBuf> cap_free;
uint64_t cur_cap_value = 0;   // (the submission being recorded: what its syncs' copies wait for)
uint32_t cur_cap_gen = 0;     // (its capture's generation)
bool sync_live() {
    static const bool v = getenv("RX_VK_SYNC_LIVE") != nullptr;
    return v;
}
std::mutex cap_free_mu;   // (the submitting threads take buffers from cap_free, the replay gives them back)
uint64_t cap_free_bytes = 0;   // (cap_free_mu)
// (the spare buffers: 12 and, on a card of 6 GB or less, 32 MB of them; RX_VK_CAP_SPARE_MB=<n>: that many, 0: as before.
// After a load the retired ones, 40-60 MB each, stayed as spares for good)
uint64_t cap_spare_max() {
    static const uint64_t v = [] {
        if (const char* e = getenv("RX_VK_CAP_SPARE_MB")) return e[0] && atoll(e) ? (uint64_t)atoll(e) << 20 : ~0ull;
        uint64_t used = 0, allowed = 0;
        return vram_usage(&used, &allowed) && allowed <= (6ull << 30) + (512ull << 20) ? 32ull << 20 : ~0ull;
    }();
    return v;
}
void cap_buf_free(VkBuffer buffer, VmaAllocation alloc, void* map, uint64_t size) {
    std::lock_guard lk(cap_free_mu);
    if (cap_free.size() < 12 && size <= (64ull << 20) && cap_free_bytes + size <= cap_spare_max()) {
        cap_free.push_back({buffer, alloc, map, size});
        cap_free_bytes += size;
    } else {
        destroy_buffer(buffer, alloc);
    }
}
void cap_buf_retire(std::map<uint32_t, CapBuf>::iterator it) {
    cap_bytes -= it->second.size;
    cap_buf_free(it->second.buffer, it->second.alloc, it->second.map, it->second.size);
    cap_bufs.erase(it);
}
// (a capture's buffer by its size class: 2, 4, 8, then 16 MB and up to the MB. It was never under 16 MB, for captures of ~1 MB:
// 25 kept ones were 413 MB of host-cached system memory, all resident, and three to a 64 MB block left 14 MB of each unused.
// RX_VK_CAP_MIN_MB=<n>: the smallest (2), 16: as before. From 16 MB each has a memory object of its own: RX_VK_CAP_BLOCKS=1: not)
uint64_t cap_buf_size(uint64_t need) {
    static const uint64_t min = (getenv("RX_VK_CAP_MIN_MB") ? (uint64_t)std::max(1, atoi(getenv("RX_VK_CAP_MIN_MB"))) : 2u) << 20;
    uint64_t s = min;
    while (s < need && s < (16ull << 20)) s <<= 1;
    return s >= need ? s : (need + (1u << 20) - 1) & ~((1ull << 20) - 1);
}
bool cap_buf_get(uint64_t need, VkBuffer& buffer, VmaAllocation& alloc, void*& map, uint64_t& size) {
    std::lock_guard lk(cap_free_mu);
    size_t best = cap_free.size();
    for (size_t i = 0; i < cap_free.size(); i++)
        if (cap_free[i].size >= need && (best == cap_free.size() || cap_free[i].size < cap_free[best].size)) best = i;
    if (best < cap_free.size()) {
        buffer = cap_free[best].buffer, alloc = cap_free[best].alloc, map = cap_free[best].map, size = cap_free[best].size;
        cap_free_bytes -= size;
        cap_free.erase(cap_free.begin() + (ptrdiff_t)best);
        return map != nullptr;
    }
    size = cap_buf_size(need);
    map = nullptr;
    return create_buffer_cached(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, buffer, alloc, &map,
                                size >= (16ull << 20) && !getenv("RX_VK_CAP_BLOCKS")) && map;
}
std::vector<std::pair<uint64_t, uint64_t>> cap_ranges;   // (the mirrored pools the captures take, as of the last one)
std::mutex cap_ranges_mu;
uint64_t cap_ranges_ver = 1;   // (any of the captures' range lists changed: cap_ranges_mu)
// (and the game's pools up to RX_VK_CAPTURE_POOL_MB (default 64; 0: none) from when they are made, mirrored or not:
// pools are imported, and mirrored, at the replay's first binding of them, two presents behind the game, and the
// captures taken before that left them out. MK World moves its per-frame data to a new pool every so many frames:
// the new pool's mirror was filled and synced from the game's memory as it was at the replay, its next frames' data
// in it already, until a mirror made the slow way published the list again (seconds): shadows dark below a line,
// the picture cyan below one, in bursts every 30 or 60 frames. The bigger pools hold textures, whose storage the
// capture takes anyway.)
// (RX_VK_CAPTURE_EAGER=<n>: only for their first n captures. Off: pools made at the load are bound by the menus much
// later, and they flashed (30 in the main menu); and the pages the captures copy are almost all the mirrored pools'
// anyway)
std::vector<std::pair<uint64_t, uint64_t>> pool_cap_ranges;   // (merged; cap_ranges_mu)
uint32_t pool_cap_gen = 0;
uint64_t captures_made = 0, pool_cap_built = 0;              // (cap_ranges_mu)
std::unordered_map<uint64_t, uint64_t> pool_first_seen;     // (a pool's start -> the capture it was first seen at)
uint64_t capture_pool_max() {
    static const uint64_t v = (getenv("RX_VK_CAPTURE_POOL_MB") ? (uint64_t)atoll(getenv("RX_VK_CAPTURE_POOL_MB")) : 64) << 20;
    return v;
}
uint64_t eager_captures() {
    static const uint64_t v = getenv("RX_VK_CAPTURE_EAGER") ? (uint64_t)atoll(getenv("RX_VK_CAPTURE_EAGER")) : 0;
    return v;
}
void refresh_pool_ranges_locked() {   // (cap_ranges_mu held)
    if (!capture_pool_max()) return;
    const uint32_t g = host->maps_generation();
    if (g == pool_cap_gen && captures_made - pool_cap_built < 64) return;   // (and every 64: the old ones out)
    pool_cap_gen = g, pool_cap_built = captures_made;
    static std::vector<std::array<uint64_t, 2>> list(8192);
    const int n = host->gpu_pools(reinterpret_cast<uint64_t(*)[2]>(list.data()), (int)list.size());
    std::vector<std::pair<uint64_t, uint64_t>> v;
    std::unordered_map<uint64_t, uint64_t> seen;
    seen.reserve((size_t)std::max(n, 0));
    for (int i = 0; i < n; i++) {
        if (!list[i][1] || list[i][1] > capture_pool_max() || list[i][0] >= (1ull << 40)) continue;
        auto it = pool_first_seen.find(list[i][0]);
        const uint64_t first = it != pool_first_seen.end() ? it->second : captures_made;
        seen[list[i][0]] = first;
        if (!eager_captures() || captures_made - first < eager_captures())
            v.push_back({list[i][0] & ~4095ull, (list[i][0] + list[i][1] + 4095) & ~4095ull});
    }
    pool_first_seen.swap(seen);
    cap_ranges_ver++;
    std::sort(v.begin(), v.end());
    std::vector<std::pair<uint64_t, uint64_t>> m;
    for (const auto& r : v) {
        if (!m.empty() && r.first <= m.back().second) m.back().second = std::max(m.back().second, r.second);
        else m.push_back(r);
    }
    pool_cap_ranges.swap(m);
}
bool capture_on() {
    static const bool on = [] {
        const char* e = getenv("RX_VK_CAPTURE");
        const bool v = e ? atoi(e) != 0 : true;
        logf("vk: submit-time capture of written pages %s", v ? "on" : "off");
        return v;
    }();
    return on;
}
std::atomic<bool> captures_seen{false};
std::atomic<uint64_t> cap_view_gen{1};   // (bumped when a capture is published or a page is stamped: cached reads go stale)
void cap_view_bump() { cap_view_gen.fetch_add(1, std::memory_order_release); }
double st_tk_all, st_tk_stamp, st_tk_copy, st_tk_late, st_tk_lock;   // (memory_take_capture's parts, ms)
// (Triforce) take_capture: the late pages not waited for (their pool had no mirror, no texture storage), by page -> the capture
// generation whose copy may hold the GPU's older words (pools_mu held); and the counts, for the takes' log line
std::unordered_map<uint32_t, uint32_t> late_skipped_gen;
int64_t st_late_skipped, st_late_needed, st_late_same;
// (Triforce) take_capture: a late page of a mirrored pool as it was when it was last waited for and synced (its bytes, and the
// capture generation that stamped it: another capture's stamp since means it was synced with other bytes) (pools_mu held)
struct LatePrev {
    uint32_t gen = 0;
    std::array<uint8_t, 4096> bytes;
};
std::unordered_map<uint32_t, LatePrev> late_prev;
bool late_same_on() {
    static const bool v = !getenv("RX_VK_LATE_SAME") || atoi(getenv("RX_VK_LATE_SAME")) != 0;
    return v;
}
// (Triforce) take_capture: a late page of a mirrored pool only part of which a submission on the GPU writes back. Its mirror sync
// copies the rest of the page alone (keep: [lo, hi) each), from the copy taken at its submit, with no wait: the part being
// written back keeps the GPU's words in the mirror, which is what the wait and the second copy were for. OoT's two per-frame
// pools hold the GPU's output buffers (16 to 40 KB each, at the same addresses every frame, not page-aligned) beside the
// uniform blocks the CPU fills every frame: the first and last page of each buffer, ~60 pages a frame, were late every frame
// while the pool had a mirror. What the CPU stores inside a range a submission on the GPU still writes is not brought to the
// mirror for such a page (on the console the GPU's write lands after it); a page written back whole waits as before.
// By page -> the capture generation it holds for (pools_mu held). RX_VK_LATE_PART=0: those pages waited for, as before
struct LatePartial {
    static constexpr int kRanges = 6;
    uint32_t gen = 0;
    uint8_t n = 0;
    uint16_t keep[kRanges + 1][2];
};
std::unordered_map<uint32_t, LatePartial> late_partial;
int64_t st_late_part;
// (Triforce) take_capture: a late page of a mirrored pool that a submission on the GPU writes back whole, and that the CPU
// changed: OoT's exposure meter (three buffers of 0x1040 bytes in turn: the GPU adds a frame's light into one, the CPU keeps a
// header in its first page), late at the first submit of every frame. Its sync is put off instead of waited for: the page is
// left out of this capture and synced from guest memory, whole, by the first capture taken once no submission on the GPU
// writes it (late_deferred_ready), as the wait and the second copy did. A binding that reaches the page before that waits for
// its write-back and syncs it then (late_deferred_bound): the old order, only when the GPU is about to use the page. The meter
// a frame wrote is not bound again for two frames, so nothing waits. Not with a parallel replay (its workers bind on their own
// threads). RX_VK_LATE_DEFER=0: waited for at the take, as before
std::mutex late_defer_mu;
std::vector<uint32_t> deferred_late;          // (pages; late_defer_mu held)
std::atomic<int> deferred_late_n{0};          // (its size, for gpu_buffer_bind's check)
int64_t st_late_deferred, st_late_jit, st_late_ready;
bool late_defer_on() {
    // (off unless asked for: a page put off is left out of its capture, so the replay's reads of it as submitted and its later
    // sync take guest memory as it is by then, the game's next frame: brightness steps at the Castle Town save, 85 cell blinks
    // per 100 frames against 0. RX_VK_LATE_DEFER=1: on, for tests)
    static const bool v = getenv("RX_VK_LATE_DEFER") && atoi(getenv("RX_VK_LATE_DEFER")) != 0;
    return v;
}
int64_t st_late_why[3];   // (waited for after all: ranges not known, none on the GPU now, written back whole)
bool late_part_on() {
    static const bool v = !getenv("RX_VK_LATE_PART") || atoi(getenv("RX_VK_LATE_PART")) != 0;
    return v;
}
int64_t st_tk_late_n;   // (the takes that waited for the GPU for their late pages)   // (a capture was ever made: the replay thread's; without it nothing owns pages)

// ---- texture storage in the capture ----
// The textures' storage the replay uploads from joins the capture, and what the capture took of it is kept (the
// shadow) for the uploads to read instead of guest memory: the storage as it was at its submit. MKW writes its terrain's
// height tiles (68 x 68 RGBA16, at new addresses every frame) for a frame and then rewrites them right after the
// submit; uploaded when the replay recorded the submit, from guest memory as it was by then, a frame's tiles could take
// another's heights. Measured at a snowy spot it is rare: of 107,000 pages the uploads read from the copies, 13 had
// been rewritten since their submit. In a Grand Prix it is what flashed: ~100 one-frame flashes a minute (giant
// stretched shapes over most of the picture, black or fog-coloured bands, a 30-frame rhythm), 9-18 rewritten pages of
// 800,000, and none with it on. On by default now that the guarded copy holds (a call, not inlined). (A page the game
// wrote and then freed is still marked: the copy commits it again, zeros, on first touch (commit_on_touch), which is
// what the mirrors must see of it; clearing the marks at the free lost that, and a Grand Prix loaded to a black
// screen.) RX_VK_TEX_CAPTURE=0: off.
bool tex_capture_on() {
    static const bool on = [] {
        const char* e = getenv("RX_VK_TEX_CAPTURE");
        const bool v = (e ? atoi(e) != 0 : true) && capture_on();
        logf("vk: texture storage in the submit-time capture %s", v ? "on" : "off");
        return v;
    }();
    return on;
}
std::vector<std::pair<uint64_t, uint64_t>> tex_cap_ranges;   // (texture storage the capture takes, merged; cap_ranges_mu)
std::mutex shadow_mu;   // (after pools_mu, before cap_ranges_mu)
struct ShadowPage {
    std::unique_ptr<uint8_t[]> b;
    uint64_t seq = 0;   // (the capture that took it)
};
std::unordered_map<uint32_t, ShadowPage> shadow;   // guest page -> its 4 KB as the last capture taken had it
std::unordered_map<uint32_t, uint32_t> shadow_regions;   // (2 MB region -> its pages in the shadow)
std::atomic<size_t> shadow_n{0};
uint64_t shadow_seq = 0;   // (captures taken)
// (256 MB: past it the pages no capture took in the last 64 go, the oldest first: those the game rewrites every frame
// stay; if none can go, a page is read from guest memory as before)
constexpr size_t kShadowMax = 65536;
std::map<uint64_t, std::pair<uint64_t, uint64_t>> tex_spans;   // lo -> (hi, the present a texture was last uploaded from it)
std::vector<std::pair<uint64_t, uint64_t>> tex_merged;          // (tex_spans merged; shadow_mu)
bool tex_spans_changed = false;
uint64_t tex_merged_bytes = 0;

bool in_tex_merged(uint64_t guest) {   // (shadow_mu held)
    auto it = std::upper_bound(tex_merged.begin(), tex_merged.end(), std::make_pair(guest, ~uint64_t(0)));
    return it != tex_merged.begin() && guest < std::prev(it)->second;
}
void shadow_erase(uint32_t page) {   // (shadow_mu held)
    if (!shadow.erase(page)) return;
    auto r = shadow_regions.find(page >> 9);
    if (r != shadow_regions.end() && --r->second == 0) shadow_regions.erase(r);
    shadow_n--;
}
void shadow_drop_range(uint64_t lo, uint64_t hi) {   // (shadow_mu held)
    if (hi <= lo) return;
    for (uint64_t r = lo >> 21; r <= (hi - 1) >> 21; r++) {
        if (!shadow_regions.count((uint32_t)r)) continue;
        const uint64_t p0 = std::max(lo >> 12, r << 9), p1 = std::min((hi + 4095) >> 12, (r + 1) << 9);
        for (uint64_t p = p0; p < p1; p++) shadow_erase((uint32_t)p);
    }
}
/// (the spans unused for 1800 presents dropped with their shadow pages; the rest published for the capture)
void publish_tex_spans(uint64_t frame) {
    std::lock_guard lk(shadow_mu);
    for (auto it = tex_spans.begin(); it != tex_spans.end();) {
        if (frame > it->second.second + 1800) {
            shadow_drop_range(it->first, it->second.first);
            it = tex_spans.erase(it);
            tex_spans_changed = true;
        } else {
            ++it;
        }
    }
    if (!tex_spans_changed) return;
    tex_spans_changed = false;
    std::vector<std::pair<uint64_t, uint64_t>> v;
    for (const auto& [lo, e] : tex_spans) {
        if (!v.empty() && lo <= v.back().second) v.back().second = std::max(v.back().second, e.first);
        else v.push_back({lo, e.first});
    }
    tex_merged = v;
    tex_merged_bytes = 0;
    for (const auto& r : v) tex_merged_bytes += r.second - r.first;
    std::lock_guard lk2(cap_ranges_mu);
    tex_cap_ranges.swap(v);
    cap_ranges_ver++;
}
} // namespace

void memory_note_texture_upload(uint64_t guest, uint64_t size) {
    if (!size || guest >= (1ull << 40) || size > (1ull << 40) || !tex_capture_on()) return;   // (hookshot: no wrap below)
    uint64_t lo, hi;
    {   // (the pool it is in, when that is at most 256 MB; else the 2 MB regions it spans)
        std::lock_guard lk(pools_mu);
        auto it = std::upper_bound(guest_pools.begin(), guest_pools.end(), guest,
                                   [](uint64_t g, const GuestPool& p) { return g < p.lo; });
        if (it != guest_pools.begin() && guest >= std::prev(it)->lo && guest + size <= std::prev(it)->hi &&
            std::prev(it)->hi - std::prev(it)->lo <= (256ull << 20)) {
            lo = std::prev(it)->lo, hi = std::prev(it)->hi;
        } else {
            lo = guest & ~((2ull << 20) - 1), hi = (guest + size + (2ull << 20) - 1) & ~((2ull << 20) - 1);
        }
    }
    const uint64_t frame = texture_frame_no();
    std::lock_guard lk(shadow_mu);
    auto [it, fresh] = tex_spans.try_emplace(lo, hi, frame);
    if (!fresh) {
        it->second.second = frame;
        if (it->second.first >= hi) return;
        it->second.first = hi;
    }
    tex_spans_changed = true;
}

/// A replay read of one page can reuse what the last read of that page decided, while the page's write generation and
/// the capture table are the ones that decision was made from.
bool submitted_page_reuse(uint64_t page, uint32_t page_gen, uint64_t view, uint64_t cached_page, uint32_t cached_gen,
                          uint64_t cached_view) {
    return cached_view != 0 && page == cached_page && page_gen == cached_gen && view == cached_view;
}
struct SubmittedPage {   // (a page no capture holds, as of pgen and view: the read from guest memory stands)
    uint64_t page = ~0ull;
    uint32_t pgen = 0;
    uint64_t view = 0;
};
// (hookshot) 32 of them a thread, by page: a handle program reads a uniform word on one page and a storage buffer word on
// another, and with one page kept nearly every read missed and took pools_mu (program_for's answer handles). A page a
// capture holds is not kept: its words are copied from the capture under pools_mu at each read, not the whole 4 KB page
// into a copy of the thread's own (that copy was a third of the replay's thread in Kokiri Forest on NVIDIA Linux)
constexpr int kSubmittedPages = 32;
thread_local SubmittedPage submitted_pages[kSubmittedPages];
// (RX_VK_SUBMITTED_PAGES=1: one slot, every page in it, as before #1762)
inline SubmittedPage& submitted_page_for(uint64_t page) {   // (a page and its neighbours, and pages 32 apart, in different slots)
    static const bool one = getenv("RX_VK_SUBMITTED_PAGES") && atoi(getenv("RX_VK_SUBMITTED_PAGES")) == 1;
    return submitted_pages[one ? 0 : (page + (page >> 5) * 13) & (kSubmittedPages - 1)];
}

namespace {
// (Triforce) take_capture's late pages taken without the wait (late_partial): their copy is the one made at the submit, before
// the write-back on the GPU landed. The replay's first read of such a page as submitted waits for that write-back and takes the
// page again, which is what take_capture did for every late page before any of them was read: see late_unresolved_read
std::atomic<int> late_unresolved_n{0};
void late_unresolved_read(uint64_t page0, uint64_t page1);
}
bool memory_read_as_submitted(uint64_t gpu, void* dst, uint64_t n) {
    // (once a capture exists, every uniform word took pools_mu to ask whether its page was in one. A page in none keeps
    // that answer until a stamp or a new capture)
    if (!host->gpu_read(gpu, dst, n)) return false;
    if (!n || !capture_on() || !captures_seen.load(std::memory_order_relaxed)) return true;
    const uint64_t guest = gpu < (1ull << 40) ? gpu : host->gpu_guest(gpu);
    if (!guest) return true;
    const uint64_t page0 = guest >> kPageShift, page1 = (guest + n - 1) >> kPageShift;
    if (late_unresolved_n.load(std::memory_order_relaxed)) late_unresolved_read(page0, page1);
    const uint64_t view = cap_view_gen.load(std::memory_order_acquire);
    SubmittedPage& submitted_page = submitted_page_for(page0);
    if (page0 == page1 && submitted_page_reuse(page0, page_gen(page0), view, submitted_page.page, submitted_page.pgen,
                                               submitted_page.view))
        return true;
    static std::atomic<int64_t> reads{0}, pages_from{0};
    int64_t from = 0;
    {
        std::lock_guard lk(pools_mu);
        for (uint64_t page = guest >> kPageShift; page < (guest + n + 4095) >> kPageShift; page++) {
            const CapEntry e = cap_entry(page);
            if (!e.gen || e.gen != page_gen(page)) continue;
            auto b = cap_bufs.find(e.gen);
            if (b == cap_bufs.end() || !b->second.map) continue;
            const uint64_t a = std::max(page << kPageShift, guest), z = std::min((page + 1) << kPageShift, guest + n);
            std::memcpy((uint8_t*)dst + (a - guest), (const uint8_t*)b->second.map + (uint64_t)e.slot * 4096 + (a - (page << kPageShift)), z - a);
            from++;
        }
        if (page0 == page1 && from == 0) {   // (in no capture: guest memory stands for its next words too)
            submitted_page.page = page0;
            submitted_page.pgen = page_gen(page0);
            submitted_page.view = cap_view_gen.load(std::memory_order_relaxed);
        }
    }
    pages_from += from;
    if (++reads % 20000 == 0)
        logf("vk: replay reads of guest memory as submitted: %lld (%lld pages from the captures)", (long long)reads.load(),
             (long long)pages_from.load());
    return true;
}

uint64_t memory_capture_view() { return cap_view_gen.load(std::memory_order_acquire); }
uint32_t memory_page_stamp(uint64_t gpu) {
    const uint64_t guest = gpu < (1ull << 40) ? gpu : host->gpu_guest(gpu);
    if (!guest) return 0;
    return page_gen(guest >> kPageShift);
}

bool memory_has_shadow(uint64_t guest, uint64_t n) {
    if (!n || !shadow_n.load(std::memory_order_relaxed)) return false;
    std::lock_guard lk(shadow_mu);
    for (uint64_t r = guest >> 21; r <= (guest + n - 1) >> 21; r++) {
        if (!shadow_regions.count((uint32_t)r)) continue;
        const uint64_t p0 = std::max(guest >> 12, r << 9), p1 = std::min((guest + n + 4095) >> 12, (r + 1) << 9);
        for (uint64_t p = p0; p < p1; p++)
            if (shadow.count((uint32_t)p)) return true;
    }
    return false;
}

/// (RX_VK_TEX_CAPTURE_OFF_FILE=<path> (A/B): while that file exists the uploads read guest memory as before)
bool overlay_off() {
    static const char* path = getenv("RX_VK_TEX_CAPTURE_OFF_FILE");
    if (!path) return false;
    static std::atomic<int64_t> next{0};
    static std::atomic<bool> off{false};
    const int64_t now = GetTickCount64();
    if (now >= next.load(std::memory_order_relaxed)) {
        next.store(now + 200, std::memory_order_relaxed);
        const bool v = GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
        if (v != off.exchange(v)) logf("vk: texture storage as submitted: %s (A/B file)", v ? "OFF" : "on");
    }
    return off.load(std::memory_order_relaxed);
}
std::atomic<int64_t> st_overlaid{0}, st_differed{0};   // (pages read from the shadow; of them, ones guest memory had changed)

const uint8_t* memory_as_submitted(uint64_t guest, const uint8_t* live, uint64_t n, std::vector<uint8_t>& scratch) {
    if (!n || !shadow_n.load(std::memory_order_relaxed) || overlay_off()) return live;
    std::lock_guard lk(shadow_mu);
    bool any = false;
    for (uint64_t r = guest >> 21; r <= (guest + n - 1) >> 21; r++) {
        if (!shadow_regions.count((uint32_t)r)) continue;
        const uint64_t p0 = std::max(guest >> 12, r << 9), p1 = std::min((guest + n + 4095) >> 12, (r + 1) << 9);
        for (uint64_t p = p0; p < p1; p++) {
            auto it = shadow.find((uint32_t)p);
            if (it == shadow.end()) continue;
            if (!any) {
                any = true;
                if (scratch.data() != live) scratch.assign(live, live + n);
            }
            const uint64_t a = std::max(p << 12, guest), b = std::min((p + 1) << 12, guest + n);
            st_overlaid++;
            if (std::memcmp(scratch.data() + (a - guest), it->second.b.get() + (a - (p << 12)), b - a)) st_differed++;
            std::memcpy(scratch.data() + (a - guest), it->second.b.get() + (a - (p << 12)), b - a);
        }
    }
    static std::atomic<int64_t> served{0};
    if (any && served.fetch_add(1) % 2000 == 0)
        logf("vk: texture storage read as submitted (%lld times; %lld pages from the copies, %lld of them rewritten in "
             "guest memory since their submit; %zu pages kept, %zu spans, %llu MB)", (long long)served.load(),
             (long long)st_overlaid.load(), (long long)st_differed.load(), shadow.size(), tex_spans.size(),
             (unsigned long long)(tex_merged_bytes >> 20));
    return any ? scratch.data() : live;
}

void memory_drop_shadow(uint64_t guest, uint64_t size) {
    if (!size || !shadow_n.load(std::memory_order_relaxed)) return;
    std::lock_guard lk(shadow_mu);
    shadow_drop_range(guest, guest + size);
}

// ---- (EvilBoyce) GPU results by page: the submission that last wrote a page into guest memory ----
// For invalidate (vk_replay.cpp): a CPU read of GPU results waits until the submission that writes them there is done, as
// the console's coherent memory has them once that work is. Guest page number -> the recording's timeline value (write-backs
// from a mirror, and small bindings written in place). RX_VK_NO_READBACK_WAIT=1: none
namespace {
std::mutex wb_pages_mu;
// (each page's value: the timeline value of its last write-back, 0 for never, in chunks of kChunkPages made when a page of
// the chunk is first written back, as the page generations above. Write-backs are of imported pools only, which lie in the
// GPU's windows, so few chunks are ever made. Values are never lowered or dropped: the readers (memory_writeback_value's
// "ever written back" and its value against the GPU's progress, and the capture's wait, the same compare) read one the GPU
// is past as "written back, nothing to wait for", whatever it is. tests/test_wb_pages.py)
struct WbPages {
    std::vector<std::unique_ptr<uint64_t[]>> chunks = std::vector<std::unique_ptr<uint64_t[]>>(kPages / kChunkPages);
    bool any = false;
    void set(uint64_t page, uint64_t value) {
        if (page >= kPages) return;
        auto& c = chunks[page / kChunkPages];
        if (!c) {
            c.reset(new uint64_t[kChunkPages]);
            std::fill(c.get(), c.get() + kChunkPages, 0ull);
        }
        c[page % kChunkPages] = value;
        any = true;
    }
    uint64_t get(uint64_t page) const {
        if (page >= kPages) return 0;
        const auto& c = chunks[page / kChunkPages];
        return c ? c[page % kChunkPages] : 0;
    }
    /// the largest value of pages first..last (0: none), and whether any had one; a chunk never made is skipped whole
    uint64_t max_in(uint64_t first, uint64_t last, bool* ever) const {
        uint64_t v = 0;
        for (uint64_t p = first; p <= last && p < kPages;) {
            const uint64_t end = (p / kChunkPages + 1) * kChunkPages;   // (the first page past p's chunk)
            const auto& c = chunks[p / kChunkPages];
            if (!c) {
                p = end;
                continue;
            }
            for (; p <= last && p < end; p++)
                if (const uint64_t pv = c[p % kChunkPages]) {
                    if (ever) *ever = true;
                    v = std::max(v, pv);
                }
        }
        return v;
    }
    bool empty() const { return !any; }
} wb_pages;
struct WbRun { uint64_t lo, hi, value; };   // (a write-back's range in guest memory and its submission)
std::vector<WbRun> wb_runs_recent;          // (wb_pages_mu held)
uint64_t wb_runs_lost = 0;                  // (the ranges of submissions up to this value were dropped unseen)
void wb_pages_note(uint64_t guest_lo, uint64_t guest_hi, uint64_t value) {
    static const bool off = getenv("RX_VK_NO_READBACK_WAIT") != nullptr;
    if (off || !value || guest_hi <= guest_lo) return;
    std::lock_guard lk(wb_pages_mu);
    for (uint64_t p = guest_lo >> kPageShift; p <= (guest_hi - 1) >> kPageShift; p++) wb_pages.set(p, value);
    // (Triforce) and the range itself, while its submission may still be on the GPU: take_capture's late pages (wb_inflight_in)
    if (wb_runs_recent.size() >= 4096) {
        const uint64_t done = vk_timeline_completed();
        std::erase_if(wb_runs_recent, [&](const WbRun& r) { return r.value <= done; });
        if (wb_runs_recent.size() >= 4096) wb_runs_recent.clear(), wb_runs_lost = value;   // (none known up to this value: those pages wait)
    }
    wb_runs_recent.push_back({guest_lo, guest_hi, value});
}
/// (Triforce) The byte ranges of a page that submissions not yet done on the GPU write back (offsets in the page, sorted and
/// merged into out, at most cap of them). False when they are not known (the list was cut while such a submission was on the
/// GPU, or more than cap ranges): the caller waits, as before. (wb_pages_mu held)
bool wb_inflight_in(uint32_t page, uint64_t done, std::pair<uint16_t, uint16_t>* out, int cap, int* n) {
    *n = 0;
    if (wb_runs_lost > done) return false;
    const uint64_t lo = (uint64_t)page << kPageShift, hi = lo + 4096;
    std::pair<uint16_t, uint16_t> found[32];
    int k = 0;
    for (const WbRun& r : wb_runs_recent) {
        if (r.value <= done || r.hi <= lo || r.lo >= hi) continue;
        if (k == 32) return false;
        found[k++] = {(uint16_t)(std::max(r.lo, lo) - lo), (uint16_t)(std::min(r.hi, hi) - lo)};   // ([first, second) in the page)
    }
    std::sort(found, found + k);
    for (int i = 0; i < k; i++) {
        if (*n && found[i].first <= out[*n - 1].second) {
            out[*n - 1].second = std::max(out[*n - 1].second, found[i].second);
            continue;
        }
        if (*n == cap) return false;
        out[(*n)++] = found[i];
    }
    return true;
}
} // namespace
bool guarded_copy(void* dst, const void* src, size_t n);   // (below)
namespace {
// (Triforce) see late_unresolved_n. By page -> the capture generation whose copy was made before the page's write-back landed
std::mutex late_unres_mu;
struct LateUnres {
    uint32_t gen;      // (the capture holding the page)
    uint64_t value;    // (the submission whose write-back of the page was on the GPU when it was taken: the one to wait for.
                       // Not the page's newest by the time of the read: that may be the recording being made, which nothing
                       // submits while its thread waits: each such wait ran to vk_timeline_wait's 250 ms)
};
std::unordered_map<uint32_t, LateUnres> late_unresolved;
int64_t st_late_read, st_late_read_waited;
double st_late_read_ms;
void late_unresolved_note(uint32_t page, uint32_t gen, uint64_t value, uint32_t oldest_kept) {
    std::lock_guard lk(late_unres_mu);
    if (late_unresolved.size() > 4096) std::erase_if(late_unresolved, [&](const auto& e) { return e.second.gen < oldest_kept; });
    late_unresolved[page] = {gen, value};
    late_unresolved_n.store((int)late_unresolved.size(), std::memory_order_relaxed);
}
void late_unresolved_read(uint64_t page0, uint64_t page1) {
    for (uint64_t page = page0; page <= page1; page++) {
        uint32_t gen = 0;
        uint64_t v = 0;
        {
            std::lock_guard lk(late_unres_mu);
            auto it = late_unresolved.find((uint32_t)page);
            if (it == late_unresolved.end()) continue;
            gen = it->second.gen, v = it->second.value;
            late_unresolved.erase(it);
            late_unresolved_n.store((int)late_unresolved.size(), std::memory_order_relaxed);
        }
        st_late_read++;
        if (v > vk_timeline_completed()) {
            const auto t0 = std::chrono::steady_clock::now();
            vk_timeline_wait(v);
            st_late_read_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            st_late_read_waited++;
        }
        std::lock_guard pk(pools_mu);
        const CapEntry e = cap_entry(page);
        if (e.gen != gen) continue;   // (a newer capture holds the page)
        auto b = cap_bufs.find(e.gen);
        if (b == cap_bufs.end() || !b->second.map) continue;
        guarded_copy((uint8_t*)b->second.map + (uint64_t)e.slot * 4096, (const uint8_t*)host->guest_base() + (page << kPageShift), 4096);
        flush_mapped_range(b->second.alloc, (uint64_t)e.slot * 4096, 4096);
    }
}
} // namespace
uint64_t memory_writeback_value(uint64_t guest, uint64_t size, bool* ever) {
    uint64_t v = 0;
    if (ever) *ever = false;
    if (!size) return 0;
    std::lock_guard lk(wb_pages_mu);
    if (wb_pages.empty()) return 0;
    v = wb_pages.max_in(guest >> kPageShift, (guest + size - 1) >> kPageShift, ever);
    return v;
}

/// A copy that fails instead of faulting on a page unmapped meanwhile (structured exception handling: the common case
/// costs a memcpy; ReadProcessMemory per page was ~2.3 us each). Not inlined, and the copy a call: without
/// -fasync-exceptions clang catches a fault in a __try block only across a call, and a 4 KB memcpy inlined into the
/// block (the per-page retry) faulted past the handler: the crash that kept the texture storage out of the capture.
/// (hookshot) The capture's copy with non-temporal stores (x86-64; RX_VK_CAPTURE_NT=0: memcpy, as before). A capture's
/// buffer comes from a pool that cycles through some thirty buffers (cap_bufs, cap_free): memory last written several
/// frames ago, in no cache by now. A plain store reads each line before it overwrites it (the read for ownership: the
/// copy asks for twice the memory traffic it needs), and the copy then stays in the caches, where nothing reads it
/// again: the GPU takes the buffer from memory, and ~28 MB a frame (Kokiri Forest: ~5 MB a submit, six submits) went
/// through the L3, pushing the game's own data out of it. A non-temporal store writes the line straight to memory
/// through the write-combining buffers: no read, nothing in the caches. Whole 64-byte lines only: the head up to the
/// destination's next line and the tail are plain copies. A fault on the source (a page unmapped meanwhile) leaves
/// through guarded_copy's handler as memcpy's does: not inlined, a call inside the __try block.
#if defined(__x86_64__) || defined(_M_X64)
__attribute__((noinline)) void* copy_nt(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    if (n < 256) return std::memcpy(dst, src, n);
    if (const size_t head = (size_t)(0 - (uintptr_t)d) & 63) {
        std::memcpy(d, s, head);
        d += head, s += head, n -= head;
    }
    for (size_t lines = n >> 6; lines; lines--, d += 64, s += 64) {
        // (the source: written by the game this frame, read in order: a page ahead of the hardware's prefetch, which
        // stops at a page's end)
        _mm_prefetch((const char*)s + 512, _MM_HINT_T0);
#ifdef __AVX2__
        const __m256i a = _mm256_loadu_si256((const __m256i*)s), b = _mm256_loadu_si256((const __m256i*)(s + 32));
        _mm256_stream_si256((__m256i*)d, a);
        _mm256_stream_si256((__m256i*)(d + 32), b);
#else
        const __m128i a = _mm_loadu_si128((const __m128i*)s), b = _mm_loadu_si128((const __m128i*)(s + 16)),
                      c = _mm_loadu_si128((const __m128i*)(s + 32)), e = _mm_loadu_si128((const __m128i*)(s + 48));
        _mm_stream_si128((__m128i*)d, a);
        _mm_stream_si128((__m128i*)(d + 16), b);
        _mm_stream_si128((__m128i*)(d + 32), c);
        _mm_stream_si128((__m128i*)(d + 48), e);
#endif
    }
    _mm_sfence();   // (the stores in memory before whoever reads the buffer next: the replay's thread, the GPU)
    if (n & 63) std::memcpy(d, s, n & 63);
    return dst;
}
bool capture_nt() {
    static const bool on = !(getenv("RX_VK_CAPTURE_NT") && atoi(getenv("RX_VK_CAPTURE_NT")) == 0);
    return on;
}
void* (*volatile guarded_memcpy)(void*, const void*, size_t) = capture_nt() ? copy_nt : std::memcpy;
/// (a copy that left at a fault did not reach its fence, and the next reader is another thread)
inline void copy_fence() {
    if (capture_nt()) _mm_sfence();
}
#else
void* (*volatile guarded_memcpy)(void*, const void*, size_t) = std::memcpy;
inline void copy_fence() {}
#endif
#ifdef _WIN32
__declspec(noinline) bool guarded_copy(void* dst, const void* src, size_t n) {
    __try {
        guarded_memcpy(dst, src, n);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
    return true;
}
#else
// (linux: the guest window is one mapping that stays readable, decommitted pages read as zero: nothing to fault on)
bool guarded_copy(void* dst, const void* src, size_t n) {
    guarded_memcpy(dst, src, n);
    return true;
}
#endif
// ---- a capture's copies: on the submitting thread and two helpers, four from 12 CPU threads (RX_VK_CAPTURE_THREADS=<n>, 0-4): ~9 MB of the
// game's dynamic data a submit, four submits a frame, ~1.4 ms on the game's thread each with one ----
// (a page written and then unmapped before the submit faulted a plain copy: ReadProcessMemory fails on it instead.
// VirtualQuery per page took ~1 ms each in this address space: 22 ms a submit for 18 pages) (runs of adjacent pages
// in one copy each, page by page only where a run fails; page i into slot i)
struct CopyPair { uint8_t* dst; const uint8_t* src; };   // (a 4 KB copy between two of our own buffers: copy_pairs)
struct CopyPart {
    const uint32_t* pages;
    int64_t i0, i1;
    uint8_t* out;
    uint8_t* ok;
    const CopyPair* pairs = nullptr;
    int* left = nullptr;   // (the caller's parts not done yet (copy_helpers.mu): a caller waits for its own, not another's too)
};
void copy_part(const CopyPart& w) {
    if (w.pairs) {
        for (int64_t i = w.i0; i < w.i1; i++) std::memcpy(w.pairs[i].dst, w.pairs[i].src, 4096);
        return;
    }
    uint8_t* const base = (uint8_t*)host->guest_base();   // (guest addresses: no GPU translation per page)
    const uint64_t gsize = host->guest_size();
    for (int64_t i = w.i0; i < w.i1;) {
        const uint64_t ga = (uint64_t)w.pages[i] << kPageShift;
        if (ga + 4096 > gsize) { i++; continue; }
        int64_t j = i + 1;
        while (j < w.i1 && w.pages[j] == w.pages[j - 1] + 1 && ((uint64_t)w.pages[j] << kPageShift) + 4096 <= gsize) j++;
        if (guarded_copy(w.out + (uint64_t)i * 4096, base + ga, (SIZE_T)(j - i) * 4096)) {
            std::memset(w.ok + i, 1, (size_t)(j - i));
        } else {
            for (int64_t q = i; q < j; q++)
                w.ok[q] = guarded_copy(w.out + (uint64_t)q * 4096, base + ((uint64_t)w.pages[q] << kPageShift), 4096);
            copy_fence();
        }
        i = j;
    }
}
struct CopyHelpers {
    std::mutex mu;
    std::condition_variable cv, done;
    std::deque<CopyPart> parts;
    bool started = false;
} copy_helpers;
void copy_helper_main() {
    SetThreadDescription(GetCurrentThread(), L"mkw_vk capture copy");
    for (;;) {
        CopyPart w;
        {
            std::unique_lock lk(copy_helpers.mu);
            copy_helpers.cv.wait(lk, [] { return !copy_helpers.parts.empty(); });
            w = copy_helpers.parts.front();
            copy_helpers.parts.pop_front();
        }
        copy_part(w);
        std::lock_guard lk(copy_helpers.mu);
        if (--*w.left == 0) copy_helpers.done.notify_all();
    }
}
int capture_copy_helpers() {   // (copy_pages and copy_pairs share one pool of helpers, started by the first call that splits)
    static const int helpers = getenv("RX_VK_CAPTURE_THREADS")
        ? std::clamp(atoi(getenv("RX_VK_CAPTURE_THREADS")), 0, 4)
        : (getenv("RX_VK_LOW_SPEC") ? 0 : std::thread::hardware_concurrency() >= 12 ? 4 : 2);   // (four where the threads are spare)
    return helpers;
}
/// (hookshot) whole's n copies in helpers + 1 parts: the first on the calling thread, the others queued for the helpers.
/// Under 128 copies (512 KB) all on the calling thread: waking the helpers costs more than they save. Its own parts no
/// helper has taken yet the caller runs itself once its first is done, rather than waiting behind what another caller
/// queued before them (copy_pairs runs with pools_mu and shadow_mu held, copy_pages' guest copies can take milliseconds)
void copy_split(CopyPart whole, int64_t n) {
    const int helpers = capture_copy_helpers();
    if (!helpers || n < 128) return copy_part({whole.pages, 0, n, whole.out, whole.ok, whole.pairs});
    const int k = helpers + 1;
    int left = k - 1;
    auto part = [&](int i) {
        CopyPart w = whole;
        w.i0 = n * i / k, w.i1 = n * (i + 1) / k, w.left = &left;
        return w;
    };
    {
        std::lock_guard lk(copy_helpers.mu);
        if (!copy_helpers.started) {
            copy_helpers.started = true;
            for (int i = 0; i < helpers; i++) std::thread(copy_helper_main).detach();
        }
        for (int i = 1; i < k; i++) copy_helpers.parts.push_back(part(i));
    }
    copy_helpers.cv.notify_all();
    copy_part(part(0));
    std::unique_lock lk(copy_helpers.mu);
    for (;;) {
        auto it = std::find_if(copy_helpers.parts.begin(), copy_helpers.parts.end(), [&](const CopyPart& w) { return w.left == &left; });
        if (it == copy_helpers.parts.end()) break;
        const CopyPart w = *it;
        copy_helpers.parts.erase(it);
        lk.unlock();
        copy_part(w);
        lk.lock();
        --left;
    }
    copy_helpers.done.wait(lk, [&] { return left == 0; });
}
void copy_pages(const uint32_t* pages, int64_t n, uint8_t* out, uint8_t* ok) {   // (one capture at a time: submit_order_mu)
    copy_split({pages, 0, n, out, ok}, n);
}
/// (hookshot) n 4 KB copies on the capture's copy helpers too: the texture shadow's (take_capture), thousands of pages a submit
/// for OoT's game thread, which copied them one after another
void copy_pairs(const CopyPair* pairs, int64_t n) {
    copy_split({nullptr, 0, n, nullptr, nullptr, pairs}, n);
}

std::unique_ptr<Capture> capture_make();
void shadow_free(Pool& p);
void shadow_forget(Pool& p, uint64_t at, uint64_t len);   // (the write-back's diff, below: a range its shadow no longer knows)
void memory_capture_submit() {
    std::unique_ptr<Capture> c = capture_make();
    if (!c) return;
    std::lock_guard lk(cap_mu);
    captures.push_back(std::move(c));
}
/// (the pages the CPU wrote since the last capture, copied now: a submit's capture, or memory_capture_now's)
std::unique_ptr<Capture> capture_make() {
    if (!capture_on()) return nullptr;
    const auto t0 = std::chrono::steady_clock::now();
    std::unique_ptr<Capture> c;
    {
        std::lock_guard lk(cap_mu);
        if (!spare.empty()) c = std::move(spare.back()), spare.pop_back();
    }
    if (!c) c = std::make_unique<Capture>();
    c->pages.clear(), c->copied.clear(), c->slots.clear(), c->nbytes = 0, c->late.clear(), c->late_value = 0;
    if (c->map) cap_buf_free(c->buffer, c->alloc, c->map, c->bsize), c->buffer = VK_NULL_HANDLE, c->map = nullptr;
    // (only the pools mirrored in VRAM, the ones the mirrors are filled from, and only their part of the write marks:
    // every written page was taken at first (hundreds of MB a submit where the game streams its world), and the whole
    // collection, over every GPU range, is ~20 ms: 4 of them a frame on the game's thread made it crawl. The replay's
    // collection takes the rest, as before)
    // (the list the replay publishes when it makes mirrors: pools_mu here waited ~40 ms a submit for the replay)
    static thread_local std::vector<std::pair<uint64_t, uint64_t>> mirrored;
    {   // (and the texture storage the replay uploads from: see "texture storage in the capture"; and the small pools)
        std::lock_guard lk(cap_ranges_mu);
        captures_made++;
        refresh_pool_ranges_locked();
        static thread_local uint64_t ver = 0;
        if (ver != cap_ranges_ver) {   // (the three lists overlap: their union, sorted, once each time one changes)
            ver = cap_ranges_ver;
            std::vector<std::pair<uint64_t, uint64_t>> all = cap_ranges;
            all.insert(all.end(), tex_cap_ranges.begin(), tex_cap_ranges.end());
            all.insert(all.end(), pool_cap_ranges.begin(), pool_cap_ranges.end());
            std::sort(all.begin(), all.end());
            mirrored.clear();
            for (const auto& r : all) {
                if (!mirrored.empty() && r.first <= mirrored.back().second)
                    mirrored.back().second = std::max(mirrored.back().second, r.second);
                else
                    mirrored.push_back(r);
            }
        }
    }
    captures_seen.store(true, std::memory_order_relaxed);
    const auto t_lock = std::chrono::steady_clock::now();
    uint32_t* pages = nullptr;
    const int64_t n = mirrored.empty() || !host->collect_written_in ? 0
                    : host->collect_written_in(reinterpret_cast<const uint64_t(*)[2]>(mirrored.data()), (int)mirrored.size(), &pages);
    const auto t_scan = std::chrono::steady_clock::now();
    // (hookshot) A page the CPU wrote that a submission still on the GPU writes back: the copy waits for that write-back.
    // OoT's exposure meter page is written by both: the GPU adds the frame's light into it and the CPU sets the sums back to
    // 0 and fills in the rest. The CPU runs a frame ahead, and a copy taken before the write-back had landed had the GPU's
    // words from the frame before; the mirror sync of the page put them back over the GPU's newer ones, and the exposure
    // stepped for a frame (one-frame flashes, 40-120 a minute in Kokiri Forest). Only the copies of such pages wait, for the
    // last submission writing them back: not every frame's present. That wait held the game's thread (~8% of its time at a
// ~2150-draw scene), so those pages are copied again by the replay, before it uses this capture, once the submission is
// done: the replay waits instead, while the game goes on. RX_VK_CAPTURE_WAIT=1: the game's thread waits, 0: no wait
    if (n > 0) {
        static const int cap_wait = getenv("RX_VK_CAPTURE_WAIT") ? atoi(getenv("RX_VK_CAPTURE_WAIT")) : 2;
        uint64_t v = 0;
        if (cap_wait) {
            const uint64_t done = vk_timeline_completed();
            std::lock_guard lk(wb_pages_mu);
            if (!wb_pages.empty())
                for (int64_t i = 0; i < n; i++) {
                    const uint64_t pv = wb_pages.get(pages[i]);
                    if (pv > done) {
                        v = std::max(v, pv);
                        if (cap_wait == 2) c->late.push_back((uint32_t)i);
                    }
                }
        }
        if (v && cap_wait == 2) c->late_value = v;
        else if (v) vk_timeline_wait(v);
        {   // (Triforce, diagnostics) RX_VK_LOG_LATE=<n>: the first n captures with late pages: which pages, and how far the GPU is
            static const int log_late = getenv("RX_VK_LOG_LATE") ? atoi(getenv("RX_VK_LOG_LATE")) : 0;
            static std::atomic<int> logged{0};
            static std::atomic<int64_t> caps{0};
            const int64_t nth = ++caps;
            if (log_late && v && nth > 3000 && logged.fetch_add(1) < log_late) {
                char line[700];
                int o = 0;
                for (uint32_t i : c->late)
                    if (o < 640) o += snprintf(line + o, sizeof line - o, " %#llx", (unsigned long long)pages[i] << kPageShift);
                logf("vk: late: capture %lld: %lld pages written, %zu late (wait for value %llu, done %llu):%s", (long long)nth,
                     (long long)n, c->late.size(), (unsigned long long)v, (unsigned long long)vk_timeline_completed(), o ? line : "");
            }
        }
    }
    if (n > 0) shadow_written(pages, n);
    if (n > 0) {
        c->pages.assign(pages, pages + n);
        if ((uint64_t)n * 4096 <= (256ull << 20) &&   // (a load writes more: those are read later, as before)
            cap_buf_get((uint64_t)n * 4096, c->buffer, c->alloc, c->map, c->bsize)) {
            static thread_local std::vector<uint8_t> ok;
            ok.assign((size_t)n, 0);
            copy_pages(pages, n, (uint8_t*)c->map, ok.data());
            for (int64_t i = 0; i < n; i++)
                if (ok[i]) c->copied.push_back(pages[i]), c->slots.push_back((uint32_t)i);
            c->nbytes = (size_t)n * 4096;
            flush_mapped(c->alloc, c->nbytes);
        }
    }
    if (pages) host->free_pages(pages);
    {   // (what it cost, every 600 submits)
        static int64_t subs, written, copied;
        static double ms, ms_lock, ms_scan;
        static uint64_t span;
        subs++, written += n > 0 ? n : 0, copied += (int64_t)c->copied.size();
        const auto now = std::chrono::steady_clock::now();
        ms += std::chrono::duration<double, std::milli>(now - t0).count();
        ms_lock += std::chrono::duration<double, std::milli>(t_lock - t0).count();
        ms_scan += std::chrono::duration<double, std::milli>(t_scan - t_lock).count();
        if (subs % 600 == 0) {
            span = 0;
            for (const auto& r : mirrored) span += r.second - r.first;
            logf("vk: capture: a submit %.2f ms (lock %.2f, scan %.2f over %zu ranges, %llu MB), %.0f pages written, %.0f "
                 "copied (%.1f MB)", ms / 600, ms_lock / 600, ms_scan / 600, mirrored.size(),
                 (unsigned long long)(span >> 20), written / 600.0, copied / 600.0, copied * 4096.0 / 600 / 1048576);
            ms = ms_lock = ms_scan = 0, written = copied = 0;
        }
    }
    return c;
}

/// The ranges the captures take (the mirrored pools, the texture storage, the small pools): their union, sorted and
/// merged, as of now; empty while no capture was made (memory_collect_written: a page in them written after its
/// submit's capture is left for the next capture). Built again only when one of the three lists changed, as
/// capture_make keeps its own union; the reference is the calling thread's until its next call
const std::vector<std::pair<uint64_t, uint64_t>>& capture_ranges_merged() {
    static const std::vector<std::pair<uint64_t, uint64_t>> none;
    static thread_local std::vector<std::pair<uint64_t, uint64_t>> merged;
    static thread_local uint64_t ver = ~uint64_t(0);
    // (no capture is ever made without the replay thread (RX_VK_ASYNC=0): the mirrored pools' writes were left for one
    // that never came, and the pools kept their first contents: a grey world)
    if (!capture_on() || !captures_seen.load(std::memory_order_relaxed)) return none;
    std::lock_guard lk(cap_ranges_mu);
    refresh_pool_ranges_locked();   // (a pool made since the last capture: the next one takes its pages, not this)
    if (ver != cap_ranges_ver) {
        ver = cap_ranges_ver;
        std::vector<std::pair<uint64_t, uint64_t>> all = cap_ranges;
        all.insert(all.end(), tex_cap_ranges.begin(), tex_cap_ranges.end());
        all.insert(all.end(), pool_cap_ranges.begin(), pool_cap_ranges.end());
        std::sort(all.begin(), all.end());
        merged.clear();
        for (const auto& r : all) {
            if (!merged.empty() && r.first <= merged.back().second)
                merged.back().second = std::max(merged.back().second, r.second);
            else
                merged.push_back(r);
        }
    }
    return merged;
}
void memory_recording_value(uint64_t value, uint64_t completed) {
    mem_rec_value = value;
    if (completed > mem_completed) mem_completed = completed;
}
bool take_capture(std::unique_ptr<Capture> c, uint64_t value, uint64_t completed) {
    const auto tk0 = std::chrono::steady_clock::now();
    // (Triforce) The late pages whose copy nothing needs with the GPU's newest words are not waited for. A late page's copy is
    // taken again after its write-back for two readers: the mirror sync of its pool (the copy goes over the mirror: the GPU's
    // older words over its newer ones, capture_make) and the texture shadow (uploads from memory the GPU drew into). A pool
    // bound in guest memory has no mirror to sync: the GPU reads and writes the game's memory itself, and the copy only serves
    // the replay's own reads of what the CPU wrote, which the copy has. OoT's per-frame pools (16 MB each, two in use) lose
    // their mirrors after a while in a scene (state 3), the GPU's results written in place there are noted for the readbacks
    // (wb_pages_note in gpu_buffer_bind), and from then on every frame's capture found 30 to 200 of their pages late: the game's
    // submitting thread waited for the GPU to finish the frame before, 1.8 ms a frame at 1440p and 4.6 ms at 2160p at the
    // Castle Town save on an RTX 4070 SUPER. Skipped pages are remembered by their capture's generation: a mirror made later
    // takes them from guest memory, not from that copy (mirror_make). RX_VK_LATE_ALL=1: every late page waited for, as before
    // And a late page whose bytes are the ones it had when it was last synced is left out of the capture altogether: the CPU
    // stored what was there (late_prev), so there is nothing of its to bring to the mirror, and the mirror may hold the GPU's
    // newer words. At the Castle Town save the eight late pages of a frame in mirrored pools (a counter report's 16 bytes at
    // 0x7fff4bd000, two 64-byte results at 0x108c6d6540, six pages of mesh draw arguments) had the same bytes at their submit,
    // after their write-back, and from frame to frame, standing or turning: every wait for them was for nothing. A page that
    // differs is waited for and taken again as before (the exposure meter, whose sums the GPU changes every frame).
    // RX_VK_LATE_SAME=0: every late page of a mirrored pool waited for
    static thread_local std::vector<uint32_t> late_skipped_now, late_same_now, late_waited_now, late_part_cand, late_defer_now;
    late_defer_now.clear();
    static thread_local std::vector<std::pair<uint32_t, LatePartial>> late_part_now;   // (index in c->pages, what to copy)
    static thread_local std::vector<uint8_t> dropped;   // (by index in c->pages: left out, late_same_now)
    late_skipped_now.clear(), late_same_now.clear(), late_waited_now.clear(), dropped.clear();
    late_part_cand.clear(), late_part_now.clear();
    if (c->late_value && c->map) {   // (the pages a submission still on the GPU writes back: see capture_make)
        // (EXPERIMENTAL, off: RX_VK_LATE_NEW=1 turns the rules below on. Each of them took the wait away from some late pages
        // and each brought brightness steps back at the Castle Town save, windowed, 2026-11-11: 43 to 85 cell blinks per 100
        // frames against 0, also with the pages taken again at the take without the wait (RX_VK_LATE_TEST=3) and with the
        // replay's reads taking them again (late_unresolved_read). Only the wait and the second copy kept, with the mirror sync
        // alone cut (RX_VK_LATE_TEST=2), stayed clean: the wait itself orders something the game's frame depends on, not found
        // yet. What is known is in the comments here and at late_partial, deferred_late, late_unresolved)
        static const bool late_all = !(getenv("RX_VK_LATE_NEW") && atoi(getenv("RX_VK_LATE_NEW")) != 0);
        uint64_t v = c->late_value;
        if (!late_all) {
            static thread_local std::vector<uint32_t> need;
            need.clear();
            {
                std::lock_guard pk(pools_mu);
                for (uint32_t i : c->late) {
                    const uint64_t guest = (uint64_t)c->pages[i] << kPageShift;
                    auto it = imported.upper_bound(guest);
                    const Pool* p = nullptr;
                    if (it != imported.begin() && (--it, guest >= it->second.lo && guest < it->second.hi)) p = &it->second;
                    // (a page of no imported pool: as before)
                    if (p && p->mstate != Pool::MIRROR_ON && p->mstate != Pool::MIRROR_WANTED) {
                        late_skipped_now.push_back(i);
                        continue;
                    }
                    // (the same bytes as when the page was last synced: nothing of the CPU's to bring to the mirror. See late_prev)
                    if (late_same_on() && std::binary_search(c->slots.begin(), c->slots.end(), i)) {
                        // (as last synced: the copy of the capture that stamped it last, while that is kept; else the bytes
                        // noted when it was last waited for, if no capture stamped it since)
                        // (by the captures' own entries, not the page's stamp: the GPU's write-backs stamp the page too)
                        const uint8_t* was = nullptr;
                        const CapEntry e = cap_entry(c->pages[i]);
                        if (e.gen) {
                            auto b = cap_bufs.find(e.gen);
                            if (b != cap_bufs.end() && b->second.map) was = (const uint8_t*)b->second.map + (uint64_t)e.slot * 4096;
                        }
                        if (!was && e.gen) {
                            auto pv = late_prev.find(c->pages[i]);
                            if (pv != late_prev.end() && pv->second.gen == e.gen) was = pv->second.bytes.data();
                        }
                        if (was && !memcmp(was, (const uint8_t*)c->map + (uint64_t)i * 4096, 4096)) {
                            late_same_now.push_back(i);
                            continue;
                        }
                    }
                    if (late_part_on() && std::binary_search(c->slots.begin(), c->slots.end(), i)) late_part_cand.push_back(i);
                    else need.push_back(i);
                }
            }
            if (!late_skipped_now.empty() || !late_same_now.empty() || !late_part_cand.empty()) {   // (texture storage: the shadow's copy is what an upload reads: waited for and taken again, as before)
                std::lock_guard sk(shadow_mu);
                if (!tex_merged.empty()) {
                    const auto to_need = [&](uint32_t i) {
                        if (!in_tex_merged((uint64_t)c->pages[i] << kPageShift)) return false;
                        need.push_back(i);
                        return true;
                    };
                    std::erase_if(late_skipped_now, to_need);
                    std::erase_if(late_same_now, to_need);
                    std::erase_if(late_part_cand, to_need);
                }
            }
            st_late_same += (int64_t)late_same_now.size();
            if (!late_part_cand.empty()) {   // (the parts of each no submission on the GPU writes back: see late_partial)
                const uint64_t done = vk_timeline_completed();
                std::lock_guard wk(wb_pages_mu);
                for (uint32_t i : late_part_cand) {
                    std::pair<uint16_t, uint16_t> g[LatePartial::kRanges];
                    int n = 0;
                    LatePartial lp;
                    lp.n = 0;
                    // (not known, or nothing of it on the GPU any more: taken again as before; all of it written back: waited for)
                    const bool known = wb_inflight_in(c->pages[i], done, g, LatePartial::kRanges, &n);
                    if (known && n && g[0].first == 0 && g[0].second == 4096 && late_defer_on() &&
                        !par_active.load(std::memory_order_relaxed)) {   // (written back whole: put off, see deferred_late)
                        late_defer_now.push_back(i);
                        continue;
                    }
                    if (!known || !n || (g[0].first == 0 && g[0].second == 4096)) {
                        (!known ? st_late_why[0] : !n ? st_late_why[1] : st_late_why[2])++;
                        {   // (diagnostics, RX_VK_LOG_LATE: the first pages of each kind)
                            static const bool log_late = getenv("RX_VK_LOG_LATE") != nullptr;
                            static int logged[3];
                            const int why = !known ? 0 : !n ? 1 : 2;
                            if (log_late && logged[why]++ < 12)
                                logf("vk: late: page %#llx waited for: %s", (unsigned long long)c->pages[i] << kPageShift,
                                     why == 0 ? "its write-backs' ranges not known" : why == 1 ? "no range of it on the GPU now" : "written back whole");
                        }
                        need.push_back(i);
                        continue;
                    }
                    uint16_t at = 0;   // (what is kept: the page without those ranges, [lo, hi) each)
                    for (int k = 0; k < n; k++) {
                        if (g[k].first > at) lp.keep[lp.n][0] = at, lp.keep[lp.n][1] = g[k].first, lp.n++;
                        at = g[k].second;
                    }
                    if (at < 4096) lp.keep[lp.n][0] = at, lp.keep[lp.n][1] = 4096, lp.n++;
                    late_part_now.push_back({i, lp});
                }
                st_late_part += (int64_t)late_part_now.size();
            }
            if (!late_defer_now.empty()) {
                st_late_deferred += (int64_t)late_defer_now.size();
                std::lock_guard dk(late_defer_mu);
                for (uint32_t i : late_defer_now)
                    if (std::find(deferred_late.begin(), deferred_late.end(), c->pages[i]) == deferred_late.end())
                        deferred_late.push_back(c->pages[i]);
                deferred_late_n.store((int)deferred_late.size(), std::memory_order_relaxed);
            }
            v = 0;
            if (!need.empty()) {
                std::lock_guard wk(wb_pages_mu);
                for (uint32_t i : need) v = std::max(v, wb_pages.get(c->pages[i]));
            }
            st_late_skipped += (int64_t)late_skipped_now.size(), st_late_needed += (int64_t)need.size();
            {   // (diagnostics) RX_VK_LOG_LATE=<n>: the first n takes with late pages: the ones waited for, by pool and why
                static const int log_late = getenv("RX_VK_LOG_LATE") ? atoi(getenv("RX_VK_LOG_LATE")) : 0;
                static int logged;
                static int64_t takes;
                if (log_late && ++takes > 500 && logged < log_late) {
                    logged++;
                    char line[700];
                    int o = 0;
                    std::lock_guard pk(pools_mu);
                    for (uint32_t i : need) {
                        const uint64_t guest = (uint64_t)c->pages[i] << kPageShift;
                        auto it = imported.upper_bound(guest);
                        const Pool* p = nullptr;
                        if (it != imported.begin() && (--it, guest >= it->second.lo && guest < it->second.hi)) p = &it->second;
                        if (o < 640)
                            o += snprintf(line + o, sizeof line - o, " %#llx(%s)", (unsigned long long)guest,
                                          !p ? "no pool" : p->mstate == Pool::MIRROR_ON ? "mirror" : p->mstate == Pool::MIRROR_WANTED ? "wanted" : "texture");
                    }
                    logf("vk: late: take of %zu late pages: %zu waited for, %zu not (value %llu, done %llu):%s", c->late.size(), need.size(),
                         late_skipped_now.size(), (unsigned long long)v, (unsigned long long)vk_timeline_completed(), o ? line : "");
                }
            }
            c->late.assign(need.begin(), need.end());
            late_waited_now.assign(need.begin(), need.end());
            // (a page with the bytes it was last synced with stays in the capture, for the replay's reads of memory as submitted:
            // left out of it, those reads took guest memory as it was by then, the game's next frame. Only its mirror sync has
            // nothing to copy: a late_partial entry with no range to keep)
            for (uint32_t i : late_same_now) {
                LatePartial none;
                none.n = 0;
                late_part_now.push_back({i, none});
            }
            {   // (lab) RX_VK_LATE_TEST=2: those pages waited for and copied again as before, only their mirror sync cut: which
                // of the two a flicker comes from
                static const int test = getenv("RX_VK_LATE_TEST") ? atoi(getenv("RX_VK_LATE_TEST")) : 0;
                if (test == 2 && !late_part_now.empty()) {
                    for (const auto& lp : late_part_now) need.push_back(lp.first);
                    std::lock_guard wk(wb_pages_mu);
                    for (uint32_t i : need) v = std::max(v, wb_pages.get(c->pages[i]));
                    c->late.assign(need.begin(), need.end());
                }
            }
            if (!late_defer_now.empty()) {
                dropped.assign(c->pages.size(), 0);
                for (uint32_t i : late_defer_now) dropped[i] = 1;
            }
        }
        // (timed apart: the replay thread waits here for the GPU, the more often the fewer pools have a mirror. With
        // RX_VK_MIRROR_MB=1024 on an 8 GB card it was most of a capture's 2.4-3 ms, six submits a frame: Hyrule Field at
        // 35-45 fps instead of 55, issue 995)
        if (v > vk_timeline_completed()) {
            const auto tw = std::chrono::steady_clock::now();
            vk_timeline_wait(v);
            st_tk_late += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tw).count();
            st_tk_late_n++;
        }
        {   // (lab) RX_VK_LATE_TEST=3: the pages synced in part are taken again now, with no wait: what the CPU stored between the
            // submit and this take is in the copy, as the copy taken after the wait had it
            static const bool test3 = getenv("RX_VK_LATE_TEST") && atoi(getenv("RX_VK_LATE_TEST")) == 3;
            if (test3 && !late_part_now.empty()) {
                uint8_t* const base = (uint8_t*)host->guest_base();
                for (const auto& lp : late_part_now)
                    guarded_copy((uint8_t*)c->map + (uint64_t)lp.first * 4096, base + ((uint64_t)c->pages[lp.first] << kPageShift), 4096);
                flush_mapped(c->alloc, c->nbytes);
            }
        }
        if (!c->late.empty()) {
            uint8_t* const base = (uint8_t*)host->guest_base();
            // (diagnostics) RX_VK_LOG_LATE_DIFF=<n>: for n takes, each late page's bytes that differ between the copy taken at the
            // submit and the one taken now, after its write-back (what the GPU wrote meanwhile), and between this take's and the
            // take before's (what either side wrote in a frame)
            static const int log_diff = getenv("RX_VK_LOG_LATE_DIFF") ? atoi(getenv("RX_VK_LOG_LATE_DIFF")) : 0;
            static int diff_logged;
            static int64_t diff_takes;
            static std::unordered_map<uint32_t, std::vector<uint8_t>> prev_pages;
            // (RX_VK_LOG_LATE_DIFF_AFTER=<seconds after the first late take>: default 10)
            static const double diff_after = getenv("RX_VK_LOG_LATE_DIFF_AFTER") ? atof(getenv("RX_VK_LOG_LATE_DIFF_AFTER")) : 10.0;
            static const auto diff_t0 = std::chrono::steady_clock::now();
            (void)diff_takes;
            const bool diff_now = log_diff && diff_logged < log_diff &&
                                  std::chrono::duration<double>(std::chrono::steady_clock::now() - diff_t0).count() > diff_after;
            if (diff_now) diff_logged++;
            for (uint32_t i : c->late) {
                uint8_t* const dst = (uint8_t*)c->map + (uint64_t)i * 4096;
                uint8_t before[4096];
                if (diff_now) memcpy(before, dst, 4096);
                guarded_copy(dst, base + ((uint64_t)c->pages[i] << kPageShift), 4096);
                if (diff_now) {
                    const auto ranges = [](const uint8_t* a, const uint8_t* b, char* out, size_t cap) {
                        int o = 0;
                        out[0] = 0;
                        for (int w = 0; w < 4096;) {
                            if (!memcmp(a + w, b + w, 4)) { w += 4; continue; }
                            int e = w + 4;
                            while (e < 4096 && memcmp(a + e, b + e, 4)) e += 4;
                            if (o < (int)cap - 24) o += snprintf(out + o, cap - o, " %#x+%#x", w, e - w);
                            w = e;
                        }
                    };
                    char gpu_side[400], frame_side[400] = " (first)";
                    ranges(before, dst, gpu_side, sizeof gpu_side);
                    auto pv = prev_pages.find(c->pages[i]);
                    if (pv != prev_pages.end()) ranges(pv->second.data(), dst, frame_side, sizeof frame_side);
                    prev_pages[c->pages[i]].assign(dst, dst + 4096);
                    logf("vk: late diff: page %#llx: landed since the submit's copy:%s | changed since the take before:%s",
                         (unsigned long long)c->pages[i] << kPageShift, gpu_side[0] ? gpu_side : " nothing", frame_side[0] ? frame_side : " nothing");
                }
            }
            flush_mapped(c->alloc, c->nbytes);
        }
    }
    const auto tl = std::chrono::steady_clock::now();
    std::lock_guard lk(pools_mu);
    const auto tk_locked = std::chrono::steady_clock::now();
    st_tk_lock += std::chrono::duration<double, std::milli>(tk_locked - tl).count();
    cur_cap_value = value;
    cur_cap_gen = 0;
    // (captures past the last kKeep, or past the byte budget, retired once the work that read them is done)
    while (!cap_bufs.empty()) {
        auto oldest = cap_bufs.begin();
        const bool over = cap_bufs.size() > kKeep || cap_bytes > kCapBytesMax;
        if (!over || std::max(oldest->second.value, oldest->second.last_use) > completed) break;
        cap_buf_retire(oldest);
    }
    uint32_t g = 0;
    // (Triforce) the pages whose sync was put off (deferred_late), now that no submission on the GPU writes them: stamped with
    // this capture's generation and no capture's copy, so the next sync of their pool takes them from guest memory, which has
    // what the GPU wrote. One this capture took again is taken from guest memory too: its copy may be older than the write-back
    static thread_local std::vector<uint32_t> ready;
    ready.clear();
    if (deferred_late_n.load(std::memory_order_relaxed)) {
        const uint64_t done = vk_timeline_completed();
        std::lock_guard dk(late_defer_mu);
        std::lock_guard wk(wb_pages_mu);
        std::erase_if(deferred_late, [&](uint32_t page) {
            // (not one put off by this very take)
            for (uint32_t i : late_defer_now)
                if (c->pages[i] == page) return false;
            if (wb_pages.get(page) > done) return false;
            ready.push_back(page);
            return true;
        });
        deferred_late_n.store((int)deferred_late.size(), std::memory_order_relaxed);
        st_late_ready += (int64_t)ready.size();
    }
    // (Triforce, 2026-11-11) A page of texture storage whose copy holds the bytes its last copy held (the shadow's, which is what
    // the uploads read) is not this capture's: not stamped, so the textures over it are not uploaded again. After a tour of ten
    // areas the game rewrote the Market's backdrop textures (sets of 4 to 8 MB BC textures) with the same bytes every few frames,
    // for as long as one stood there (RX_VK_STAMP_SRC: "bytes THE SAME as at its last upload", stamped by a capture): 27 to
    // 73 MB of uploads a second, the frames with one 21.6 ms against 15.6 to 17.2, 53 to 42 fps against 57 to 61 without. A
    // write the captures did not take, the GPU's included, drops the shadow's copy (memory_stamp, shadow_erase): such a page is
    // stamped as before. RX_VK_TEX_SAME=0: every copied page stamped, as before
    // The page is stamped and kept in the capture as any other (the mirrors' syncs and the replay's reads as submitted see what
    // they saw); only the textures' question, "written since my upload?" (memory_written_runs for a texture: page_gen_tex),
    // takes such a stamp for the generation the page had before it. RX_VK_TEX_SAME=0: as before
    static const bool tex_same = !(getenv("RX_VK_TEX_SAME") && !atoi(getenv("RX_VK_TEX_SAME")));
    static int64_t st_tex_same, st_tex_changed;
    static thread_local std::vector<std::pair<uint32_t, uint32_t>> same_pages;   // (the page, its generation before this take)
    same_pages.clear();
    if (tex_same && !c->copied.empty() && c->map && tex_capture_on()) {
        std::lock_guard sk(shadow_mu);
        if (!tex_merged.empty() && shadow_n.load(std::memory_order_relaxed)) {
            for (size_t i = 0; i < c->copied.size(); i++) {
                const uint32_t p = c->copied[i];
                if (!dropped.empty() && dropped[c->slots[i]]) continue;
                if (!in_tex_merged((uint64_t)p << 12)) continue;
                const auto s = shadow.find(p);
                if (s == shadow.end() || !s->second.b) continue;
                if (memcmp(s->second.b.get(), (const uint8_t*)c->map + (uint64_t)c->slots[i] * 4096, 4096) != 0) {
                    st_tex_changed++;
                    continue;
                }
                same_pages.push_back({p, page_gen_tex(p)});
                st_tex_same++;
            }
        }
        static int64_t takes;
        if (++takes % 6000 == 0)
            logf("vk: texture storage pages copied again: %lld with the bytes of their last copy (no upload for them), %lld changed",
                 (long long)st_tex_same, (long long)st_tex_changed);
    }
    if (!c->pages.empty() || !ready.empty()) {
        g = ++gen_now;
        stamp_src_note(g, 2);
        for (uint32_t page : ready) stamp_page(page, g);
    }
    for (const auto& [p, before] : same_pages) page_same_set(p, g, before);   // (stamped g below, with the rest)
    if (!c->pages.empty()) {
        if (dropped.empty())
            for (uint32_t p : c->pages) stamp_page(p, g);
        else   // (Triforce, above: a late page with the bytes it was last synced with is not this capture's)
            for (size_t i = 0; i < c->pages.size(); i++)
                if (!dropped[i]) stamp_page(c->pages[i], g);
        pages_collected += c->pages.size();
    }
    uint32_t late_unres_gen = 0;
    if (g && !late_part_now.empty()) {   // (Triforce: the late pages synced in part, by this capture's generation)
        if (late_partial.size() > 4096) {
            const uint32_t oldest = cap_bufs.empty() ? g : cap_bufs.begin()->first;
            std::erase_if(late_partial, [&](const auto& e) { return e.second.gen < oldest; });
        }
        for (auto& [i, lp] : late_part_now) {
            lp.gen = g;
            late_partial[c->pages[i]] = lp;
        }
        late_unres_gen = g;   // (noted once the pages have their entries, below)
    }
    if (g && c->map && late_same_on()) {   // (Triforce: the late pages waited for and taken again, as they are synced now)
        if (late_prev.size() > 2048) late_prev.clear();   // (a bound: 8 MB; the pages come back at their next wait)
        for (uint32_t i : late_waited_now) {
            if (!std::binary_search(c->slots.begin(), c->slots.end(), i)) continue;   // (its copy failed)
            LatePrev& lp = late_prev[c->pages[i]];
            lp.gen = g;
            memcpy(lp.bytes.data(), (const uint8_t*)c->map + (uint64_t)i * 4096, 4096);
        }
    }
    if (!late_skipped_now.empty() && g) {   // (Triforce, above: the copies that may hold the GPU's older words, by this capture's generation)
        if (late_skipped_gen.size() > 4096) {
            const uint32_t oldest = cap_bufs.empty() ? g : cap_bufs.begin()->first;   // (older than every kept capture: of no use)
            std::erase_if(late_skipped_gen, [&](const auto& e) { return e.second < oldest; });
        }
        for (uint32_t i : late_skipped_now) late_skipped_gen[c->pages[i]] = g;
    }
    if (!c->pages.empty() && tex_capture_on()) {   // (the texture storage it took: the shadow, as of this submit)
        std::lock_guard sk(shadow_mu);
        shadow_seq++;
        // (the copies of pages no capture took in the last 256 submits go, over 32 MB of them, every 256 captures: such a page is what guest
        // memory holds (a write drops its copy), and the cap's sweep below came only at 256 MB. RX_VK_SHADOW_SWEEP_OFF=1: the cap's alone)
        static const bool sweep_off = getenv("RX_VK_SHADOW_SWEEP_OFF") != nullptr;
        if (!sweep_off && shadow_seq % 256 == 0 && shadow_n.load(std::memory_order_relaxed) > 8192)
            for (auto it = shadow.begin(); it != shadow.end();) {
                if (it->second.seq + 256 >= shadow_seq) { ++it; continue; }
                auto r = shadow_regions.find(it->first >> 9);
                if (r != shadow_regions.end() && --r->second == 0) shadow_regions.erase(r);
                it = shadow.erase(it);
                shadow_n--;
            }
        if (!tex_merged.empty()) {
            // (every page written since the last capture: its old copy is stale; the ones copied now replace it)
            // (hookshot: those copied now are overwritten in place below, their buffers kept: erasing every written page and
            // inserting the copied ones again was a free, an allocation and two hash operations a page, for the pages the
            // game rewrites every frame. c->slots[i] is the index in c->pages of c->copied[i], in order)
            {
                size_t j = 0;
                for (size_t i = 0; i < c->pages.size(); i++) {
                    if (j < c->slots.size() && c->slots[j] == i) {
                        j++;
                        continue;
                    }
                    const uint32_t p = c->pages[i];
                    if (in_tex_merged((uint64_t)p << 12)) shadow_erase(p);
                }
            }
            bool swept = false;
            static thread_local std::vector<CopyPair> pairs;   // (the copies, made together once the slots are known)
            pairs.clear();
            size_t i = 0;
            for (; i < c->copied.size(); i++) {
                const uint32_t p = c->copied[i];
                if (!in_tex_merged((uint64_t)p << 12)) continue;
                if (shadow_n.load(std::memory_order_relaxed) >= kShadowMax && !shadow.count(p)) {
                    if (swept) break;
                    swept = true;
                    for (auto it = shadow.begin(); it != shadow.end();) {
                        if (it->second.seq + 64 >= shadow_seq) { ++it; continue; }
                        auto r = shadow_regions.find(it->first >> 9);
                        if (r != shadow_regions.end() && --r->second == 0) shadow_regions.erase(r);
                        it = shadow.erase(it);
                        shadow_n--;
                    }
                    if (shadow_n.load(std::memory_order_relaxed) >= kShadowMax) break;
                }
                ShadowPage& slot = shadow[p];
                if (!slot.b) {
                    slot.b.reset(new uint8_t[4096]);
                    shadow_regions[p >> 9]++;
                    shadow_n++;
                }
                slot.seq = shadow_seq;
                pairs.push_back({slot.b.get(), (const uint8_t*)c->map + (uint64_t)c->slots[i] * 4096});
            }
            // (shadow_mu still held: no slot taken here goes before its copy; the sweep above drops only older ones)
            if (!pairs.empty()) copy_pairs(pairs.data(), (int64_t)pairs.size());
            for (; i < c->copied.size(); i++)   // (the shadow full: the copied pages not replaced lose their old copies)
                if (in_tex_merged((uint64_t)c->copied[i] << 12)) shadow_erase(c->copied[i]);
        }
    }
    const auto tk1 = std::chrono::steady_clock::now();
    if (!c->copied.empty() && g && c->map) {   // (its buffer kept with the captures, for the mirror syncs)
        CapBuf b{value, c->buffer, c->alloc, c->bsize, c->map};
        c->buffer = VK_NULL_HANDLE, c->alloc = nullptr, c->map = nullptr, c->bsize = 0;
        cap_bufs.emplace(g, b);
        cap_bytes += b.size;
        cur_cap_gen = g;
        for (size_t i = 0; i < c->copied.size(); i++)
            if (dropped.empty() || !dropped[c->slots[i]]) cap_entry_set(c->copied[i], CapEntry{g, c->slots[i]});
    }
    if (late_unres_gen) {   // (Triforce: the late pages taken without the wait: taken again at the replay's first read of each)
        static const bool test2 = getenv("RX_VK_LATE_TEST") && atoi(getenv("RX_VK_LATE_TEST")) == 2;   // (waited for at the take)
        const uint32_t oldest = cap_bufs.empty() ? late_unres_gen : cap_bufs.begin()->first;
        if (!test2)
            for (const auto& lp : late_part_now) {
                uint64_t pv = 0;
                {
                    std::lock_guard wk(wb_pages_mu);
                    pv = wb_pages.get(c->pages[lp.first]);
                }
                late_unresolved_note(c->pages[lp.first], late_unres_gen, pv, oldest);
            }
    }
    for (uint32_t page : ready) {   // (Triforce, above: from guest memory, whatever capture held it)
        const CapEntry e = cap_entry(page);
        if (e.gen) cap_entry_set(page, CapEntry{0, 0});
    }
    cap_view_bump();
    if (c->map) cap_buf_free(c->buffer, c->alloc, c->map, c->bsize), c->buffer = VK_NULL_HANDLE, c->map = nullptr;
    {
        std::lock_guard lk(cap_mu);
        if (spare.size() < 8) spare.push_back(std::move(c));
    }
    {   // (every 600 takes: where a take's time went)
        const auto tk2 = std::chrono::steady_clock::now();
        static int64_t n;
        st_tk_stamp += std::chrono::duration<double, std::milli>(tk1 - tk_locked).count();
        st_tk_all += std::chrono::duration<double, std::milli>(tk2 - tk0).count();
        if (++n % 600 == 0) {
            logf("vk: a capture taken (ms): all %.3f; GPU wait for late pages %.3f (%lld of 600 takes; %lld late pages waited "
                 "for (%lld ranges unknown, %lld none on the GPU now, %lld written back whole), %lld not: no mirror, %lld not: "
                 "the same bytes, %lld synced in part, %lld put off (%lld synced later, %lld at a binding); %lld of those taken "
                 "again at the replay's first read, %lld after a wait, %.3f ms a take), pools lock %.3f, retires "
                 "and stamps %.3f, bytes copied %.3f; %zu kept (%llu MB), %zu spare buffers, %zu texture pages copied (%llu MB)",
                 st_tk_all / 600, st_tk_late / 600, (long long)st_tk_late_n, (long long)st_late_needed, (long long)st_late_why[0],
                 (long long)st_late_why[1], (long long)st_late_why[2], (long long)st_late_skipped,
                 (long long)st_late_same, (long long)st_late_part, (long long)st_late_deferred, (long long)st_late_ready,
                 (long long)st_late_jit, (long long)st_late_read, (long long)st_late_read_waited, st_late_read_ms / 600,
                 st_tk_lock / 600, st_tk_stamp / 600, st_tk_copy / 600,
                 cap_bufs.size(), (unsigned long long)(cap_bytes >> 20), cap_free.size(), shadow_n.load(std::memory_order_relaxed),
                 (unsigned long long)(shadow_n.load(std::memory_order_relaxed) >> 8));
            st_tk_all = st_tk_stamp = st_tk_copy = st_tk_late = st_tk_lock = 0, st_tk_late_n = 0;
            st_late_needed = st_late_skipped = st_late_same = st_late_part = 0;
            st_late_why[0] = st_late_why[1] = st_late_why[2] = 0;
            st_late_deferred = st_late_ready = st_late_jit = 0;
            st_late_read = st_late_read_waited = 0, st_late_read_ms = 0;
        }
    }
    return true;
}
bool memory_take_capture(uint64_t value, uint64_t completed) {
    if (completed > mem_completed) mem_completed = completed;
    if (!capture_on()) return false;
    std::unique_ptr<Capture> c;
    {
        std::lock_guard lk(cap_mu);
        if (captures.empty()) return false;
        c = std::move(captures.front());
        captures.pop_front();
    }
    return take_capture(std::move(c), value, completed);
}
/// (hookshot) A capture made and taken at once, for a submission going on from a wait the CPU's signal met
/// (vk_replay.cpp resume_queue): the CPU's writes up to that signal, which the console's GPU reads after its wait
bool memory_capture_now(uint64_t value, uint64_t completed) {
    if (completed > mem_completed) mem_completed = completed;
    std::unique_ptr<Capture> c = capture_make();
    return c && take_capture(std::move(c), value, completed);
}

namespace {

/// The imported pool holding [guest, guest + size) (imported now if it was not yet); null outside the pools.
/// pools_mu held.
Pool* pool_cache[4096];   // (by 64 KB page: the pools buffer bindings hit lately)
std::atomic<uint64_t> pools_gen{1};   // (bumped when a pool leaves the table: the recording threads' caches go stale)
struct TlPool {
    Pool* p;
    uint64_t gen;
};
thread_local TlPool tl_pools[1024];   // (a parallel replay thread's: by 64 KB page, read without the lock)

Pool* pool_of(uint64_t guest, uint64_t size) {
    if (!size || size > UINT64_MAX - guest) return nullptr;
    if (last_pool && guest >= last_pool->lo && guest + size <= last_pool->hi) return last_pool;
    Pool*& slot = pool_cache[(guest >> 16) & 4095];
    if (slot && guest >= slot->lo && guest + size <= slot->hi) return last_pool = slot;
    Pool* found = pool_lookup(guest, size);
    if (found) slot = found;
    return found;
}

// ---- pools imported in the background: an import took 5-45 ms (the driver locks the pages; the game's threads keep
// the address space busy committing theirs), and the game makes ~13 pools a second. The host says when the game makes
// one (memory_pool_created), a thread of ours imports it, and a lookup that finds one under way waits for it rather
// than importing it again. Imports made are handed over in a list (the lookup holds pools_mu, the thread never). ----
std::mutex bg_mu;
std::condition_variable bg_cv, bg_done_cv;
std::deque<std::pair<uint64_t, uint64_t>> bg_queue;
std::vector<uint64_t> bg_inflight;           // (lo of the ranges queued or being imported)
std::vector<std::pair<uint64_t, Pool>> bg_ready;
int64_t st_bg_imports, st_bg_waits, st_prefetch_sent, st_prefetch_used, st_prefetch_dropped;
void bg_main();
void bg_start() {   // (bg_mu held)
    static bool started;
    if (!started) {
        started = true;
        std::thread(bg_main).detach();
    }
}

void bg_main() {
    SetThreadDescription(GetCurrentThread(), L"mkw_vk imports");
    for (;;) {
        std::pair<uint64_t, uint64_t> r;
        {
            std::unique_lock lk(bg_mu);
            bg_cv.wait(lk, [] { return !bg_queue.empty(); });
            r = bg_queue.front();
            bg_queue.pop_front();
        }
        Pool p;
        const bool ok = import_pool(r.first, r.second, p);
        std::lock_guard lk(bg_mu);
        p.prefetched = true;
        if (ok) bg_ready.push_back({r.first, p}), st_bg_imports++;
        std::erase(bg_inflight, r.first);
        bg_done_cv.notify_all();
    }
}

/// The imports the thread made, into the table (pools_mu held); one already there makes the new one redundant
void bg_drain() {
    std::vector<std::pair<uint64_t, Pool>> ready;
    {
        std::lock_guard lk(bg_mu);
        if (bg_ready.empty()) return;
        ready.swap(bg_ready);
    }
    for (auto& [lo, p] : ready) {
        auto imp = imported.find(lo);
        if (imp != imported.end() && imp->second.hi >= p.hi) {
            vkDestroyBuffer(dev.device, p.buffer, nullptr);
            vkFreeMemory(dev.device, p.memory, nullptr);
            for (VkDeviceMemory m : p.pieces) vkFreeMemory(dev.device, m, nullptr);
            continue;
        }
        bool overlap = false;   // (amd-fix: a chunk imported ahead that an import made since covers in part: the pools stay disjoint)
        for (auto o = imported.lower_bound(p.hi); o != imported.begin();) {
            --o;
            if (o == imp) continue;
            if (o->second.hi <= p.lo) {
                if (o->second.hi + (1ull << 30) < p.lo) break;
                continue;
            }
            overlap = true;
            break;
        }
        if (overlap) {
            st_prefetch_dropped++;
            vkDestroyBuffer(dev.device, p.buffer, nullptr);
            vkFreeMemory(dev.device, p.memory, nullptr);
            for (VkDeviceMemory m : p.pieces) vkFreeMemory(dev.device, m, nullptr);
            continue;
        }
        if (imp != imported.end()) retire_pool(imp);
        imported.emplace(lo, p);
        guest_pages_set(p.lo, p.hi, p.address);
    }
}

/// (amd-fix) An import on the replay thread stalled it 20-75 ms on an RX 9060 XT (the driver locks the chunk's pages, the
/// commit of fresh ones), a tiny uniform buffer's binding as long, ~40 times entering Kokiri: hitches. The chunks next to
/// one imported on demand, inside the game's pools, are imported ahead by the background thread; a lookup that needs one
/// still under way waits for it (less than importing it), and a chunk imported ahead and first bound sends for its own
/// neighbours. RX_VK_PREFETCH_CHUNKS: how many on each side (default 0: off, the background imports slowed the replay thread's own to 80-127 ms on AMD). (pools_mu held)
void prefetch_chunks(uint64_t lo, uint64_t hi, uint64_t chunk) {
    static const int n = getenv("RX_VK_PREFETCH_CHUNKS") ? std::max(0, atoi(getenv("RX_VK_PREFETCH_CHUNKS"))) : 0;
    if (n <= 0 || !chunk || !dev.external_memory_host || hi <= lo) return;
    auto in_game_pool = [](uint64_t a, uint64_t b) {
        auto g = std::lower_bound(guest_pools.begin(), guest_pools.end(), b, [](const GuestPool& p, uint64_t v) { return p.lo < v; });
        return g != guest_pools.begin() && std::prev(g)->hi > a;
    };
    auto imported_over = [](uint64_t a, uint64_t b) {
        for (auto o = imported.lower_bound(b); o != imported.begin();) {
            --o;
            if (o->second.hi > a) return true;
            if (o->second.hi + (1ull << 30) < a) break;
        }
        return false;
    };
    std::vector<std::pair<uint64_t, uint64_t>> want;
    for (int k = 0; k < n; k++) {
        const uint64_t up = hi + (uint64_t)k * chunk;
        if (up < UINT64_MAX - chunk) want.push_back({up, up + chunk});
        const uint64_t down = (uint64_t)(k + 1) * chunk;
        if (lo >= down) want.push_back({lo - down, lo - down + chunk});
    }
    std::lock_guard lk(bg_mu);
    for (const auto& [a, b] : want) {
        if (b > (1ull << 40) || !in_game_pool(a, b) || imported_over(a, b)) continue;
        if (std::find(bg_inflight.begin(), bg_inflight.end(), a) != bg_inflight.end()) continue;
        if (bg_inflight.size() >= 16) break;
        bg_start();
        bg_inflight.push_back(a);
        bg_queue.push_back({a, b});
        st_prefetch_sent++;
    }
    bg_cv.notify_one();
}

/// (imports by 16 MB chunk (RX_VK_IMPORT_CHUNK_MB), not by pool: each import cost 5-45 ms in the driver, more as their
/// number grew, and the game makes ~13 pools a second; a chunk covers the pools after it at no cost)
uint64_t import_chunk() {
    static const uint64_t chunk = (uint64_t)(getenv("RX_VK_IMPORT_CHUNK_MB") ? std::max(0, atoi(getenv("RX_VK_IMPORT_CHUNK_MB"))) : 16) << 20;
    return chunk;
}
/// (hookshot) A game pool across a chunk's end had each chunk imported on its own, at the first binding in it, and a later
/// binding across the end imported the two again as one, on the replay thread: OoT's 16 MB pool at 0x1021745000 bound at
/// 0x1021ffff00+0x160 across 0x1022000000, 130-147 ms, the one hitch of a panning run in Kokiri. Up to this size an import
/// takes the chunks of the game pools across its ends too (RX_VK_IMPORT_SPAN_MB, default 64; 0: not)
uint64_t import_span() {
    static const uint64_t span = (uint64_t)(getenv("RX_VK_IMPORT_SPAN_MB") ? std::max(0, atoi(getenv("RX_VK_IMPORT_SPAN_MB"))) : 64) << 20;
    return span;
}
/// [lo, hi), chunk-aligned, grown over the game pools that run across its ends, while it stays within import_span (pools_mu held)
void span_game_pools(uint64_t& lo, uint64_t& hi, uint64_t chunk) {
    const uint64_t span = import_span();
    if (!chunk || hi - lo >= span) return;
    for (bool grew = true; grew;) {
        grew = false;
        for (const uint64_t e : {lo, hi}) {
            // (a pool across e that fits starts above e - span: the pools from there to e, as the host's may overlap)
            auto g = std::upper_bound(guest_pools.begin(), guest_pools.end(), e, [](uint64_t v, const GuestPool& p) { return v < p.lo; });
            while (g != guest_pools.begin()) {
                --g;
                if (g->lo + span <= e) break;
                if (g->lo >= e || g->hi <= e) continue;
                const uint64_t nlo = std::min(lo, g->lo / chunk * chunk), nhi = std::max(hi, (g->hi + chunk - 1) / chunk * chunk);
                if (nhi - nlo > span || (nlo == lo && nhi == hi)) continue;
                lo = nlo, hi = nhi, grew = true;
            }
        }
    }
}

Pool* pool_lookup(uint64_t guest, uint64_t size) {
    if (guest_pools.empty()) return nullptr;
    bg_drain();
    auto it = std::upper_bound(guest_pools.begin(), guest_pools.end(), guest,
                               [](uint64_t a, const GuestPool& p) { return a < p.lo; });
    if (it == guest_pools.begin()) return nullptr;
    --it;
    if (guest < it->lo || guest >= it->hi || !size || size > it->hi - guest) return nullptr;
    const uint64_t chunk = import_chunk();
    const GuestPool range = chunk ? GuestPool{guest, guest + size} : *it;
    auto covering = [&]() {
        auto c = imported.upper_bound(range.lo);
        if (c == imported.begin()) return imported.end();
        --c;
        return c->second.lo <= range.lo && c->second.hi >= range.hi ? c : imported.end();
    };
    auto imp = chunk ? covering() : imported.find(range.lo);
    if (chunk && range.hi > UINT64_MAX - (chunk - 1)) return nullptr;
    const uint64_t want_lo = chunk ? range.lo / chunk * chunk : range.lo;
    const uint64_t want_hi = chunk ? (range.hi + chunk - 1) / chunk * chunk : range.hi;
    if (chunk && imp == imported.end()) imp = imported.find(want_lo);   // (a smaller one there: made again, larger)
    if (imp == imported.end() || imp->second.hi < range.hi) {   // (under way in the background: waited for)
        bool waited = false;
        {
            std::unique_lock lk(bg_mu);
            auto under_way = [&] {   // (the pool's own import, or a chunk of the range imported ahead)
                for (const uint64_t x : bg_inflight)
                    if (x == range.lo || (chunk && x >= want_lo && x < want_hi)) return true;
                return false;
            };
            if (under_way()) {
                bg_done_cv.wait(lk, [&] { return !under_way(); });
                waited = true;
                st_bg_waits++;
            }
        }
        if (waited) {
            bg_drain();
            imp = chunk ? covering() : imported.find(range.lo);
            if (chunk && imp == imported.end()) imp = imported.find(want_lo);
        }
    }
    if (imp == imported.end() || imp->second.hi < range.hi || imp->second.lo > range.lo) {
        // (the new pool takes in every imported pool its range overlaps, and they are retired: the imported pools stay
        // disjoint. A range crossing a chunk's end made the chunk's pool again, larger, over the next chunk, whose own
        // pool stayed: two mirrors of the same memory. MKW's GPU culling wrote its instance data (a 2.4 MB storage
        // buffer) through one, the draws read it through the other, synced from guest memory before the submission's
        // write-back: the instances of a frame before or older, objects popping in and out, one-frame flashes.)
        uint64_t lo = 0, hi = 0;
        std::vector<uint64_t> overlapped;
        auto plan = [&](bool spanned) {   // (the range, the imported pools it takes in; spanned: span_game_pools's chunks too)
            lo = want_lo, hi = std::max(want_hi, imp != imported.end() ? imp->second.hi : 0);
            if (spanned && chunk) span_game_pools(lo, hi, chunk);
            for (bool grew = true; grew;) {
                grew = false;
                overlapped.clear();
                auto it = imported.lower_bound(hi);   // (the pools starting below hi, the last ones first)
                while (it != imported.begin()) {
                    --it;
                    if (it->second.hi <= lo) {
                        if (it->second.hi + (1ull << 30) < lo) break;   // (imports are at most 1 GB: none further down reaches lo)
                        continue;
                    }
                    overlapped.push_back(it->first);
                    if (it->second.lo < lo) lo = it->second.lo, grew = true;
                    if (it->second.hi > hi) hi = it->second.hi, grew = true;
                }
            }
            if (hi - lo > (1ull << 30)) {   // (no larger than 1 GB: then as before, the chunk's pool alone, overlaps logged)
                static int logged_big;
                if (logged_big++ < 8)
                    logf("vk: pool %#llx..%#llx would take in %zu pools, %llu MB: left overlapping", (unsigned long long)lo,
                         (unsigned long long)hi, overlapped.size(), (unsigned long long)((hi - lo) >> 20));
                lo = want_lo, hi = std::max(want_hi, imp != imported.end() ? imp->second.hi : 0);
                overlapped.clear();
                if (imp != imported.end()) overlapped.push_back(imp->first);
            }
        };
        plan(true);
        const uint64_t spanned_lo = lo, spanned_hi = hi;
        Pool p;
        bool ok = false;
        for (int pass = 0; pass < 2 && !ok; pass++) {
            if (pass) {   // (a spanned import that failed: the range without the span, as imported before)
                plan(false);
                if (lo == spanned_lo && hi == spanned_hi) break;
            }
            const auto t0 = std::chrono::steady_clock::now();
            ok = import_pool(lo, hi, p);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            static double total_ms;
            static int n, logged;
            total_ms += ms, n++;
            if ((ms >= 2.0 && logged++ < 200) || n % 256 == 0)
                logf("vk: pool %#llx+%#llx imported in %.1f ms as %#llx..%#llx (%d imports, %.0f ms in all)",
                     (unsigned long long)range.lo, (unsigned long long)(range.hi - range.lo), ms, (unsigned long long)lo,
                     (unsigned long long)hi, n, total_ms);
        }
        if (!ok) {
            if (!import_cooling && failures++ < 16) {
                if (import_result != VK_SUCCESS)
                    logf("vk: pool %#llx+%#llx not imported (VkResult %d)", (unsigned long long)range.lo,
                         (unsigned long long)(range.hi - range.lo), (int)import_result);
                else
                    logf("vk: pool %#llx+%#llx not imported", (unsigned long long)range.lo,
                         (unsigned long long)(range.hi - range.lo));
            }
            return nullptr;
        }
        for (uint64_t k : overlapped) {   // (the pools it took in)
            auto o = imported.find(k);
            if (o != imported.end()) retire_pool(o);
        }
        imp = imported.emplace(p.lo, p).first;
        guest_pages_set(p.lo, p.hi, p.address);
        if (chunk) prefetch_chunks(p.lo, p.hi, chunk);
        if (overlapped.size() > 1) {
            static int logged_merge;
            if (logged_merge++ < 16)
                logf("vk: pool %#llx..%#llx took in %zu pools", (unsigned long long)p.lo, (unsigned long long)p.hi,
                     overlapped.size());
        }
        static int logged;
        if (logged++ < 8 || imported.size() % 256 == 0) logf("vk: %zu pools imported", imported.size());
    }
    if (imp->second.prefetched) {   // (first bound: its neighbours ahead in turn)
        imp->second.prefetched = false;
        st_prefetch_used++;
        prefetch_chunks(imp->second.lo, imp->second.hi, chunk);
        static int64_t logged_at;
        if (st_prefetch_used / 16 != logged_at) {
            logged_at = st_prefetch_used / 16;
            logf("vk: chunks imported ahead: %lld sent, %lld bound, %lld dropped; %lld waits", (long long)st_prefetch_sent,
                 (long long)st_prefetch_used, (long long)st_prefetch_dropped, (long long)st_bg_waits);
        }
    }
    imp->second.range_lo = range.lo, imp->second.range_hi = range.hi;
    return last_pool = &imp->second;
}

// ---- the VRAM mirror (after totk-pc-recomp's vk_memory.c) ----
// An imported pool keeps vertices, indices, constants and storage buffers in system memory, so a discrete GPU reads
// them over PCIe on every draw (TOTK measured ~110 ms of GPU time a frame on a 3090 Ti). A pool bound as a buffer
// gets a device-local copy, made at the start of the next submission, and from then on its bindings use the copy at
// the same offsets:
// - CPU -> mirror: at a submission's first use of a mirror, the pages the CPU wrote since its last sync (the page
//   write generations) are copied in, in the recording's upload command buffer (submitted right before it).
// - GPU -> guest: what the submission's shaders and copies may write into a mirror is copied back into the imported
//   pool at the submission's end, so the CPU (and everything that reads guest memory) sees it.
// RX_VK_MIRROR=0: no mirrors; RX_VK_MIRROR_MB: their budget (default 3072).
bool mirror_on() {
    static const int on = [] {
        const char* e = getenv("RX_VK_MIRROR");
        const bool discrete = dev.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        const int v = e ? atoi(e) != 0 : discrete;
        logf("vk: VRAM mirror of buffer pools %s", v ? "on" : "off");
        return v;
    }();
    return on != 0;
}

/// (hookshot, smoothness) A read binding in a chunk not imported yet: its import goes to the background thread and the binding is
/// served from a copy in the recording's stream, rather than the replay waiting for the import. Imports on the replay thread were
/// the main hitch left in play (54-61 a run of the verification walks, 5-380 ms each, 2026-11-09). True: an import of the range is
/// under way (sent now or before) and nothing covers it yet; false: the synchronous path as before (covered, outside the game's
/// pools, or next to an import that would have to grow). RX_VK_ASYNC_IMPORT=1 (opt-in: background imports slowed AMD before) (pools_mu held)
int64_t st_async_sent, st_async_copies;
bool import_in_background(uint64_t guest, uint64_t size) {
    if (guest_pools.empty() || !size || !dev.external_memory_host) return false;
    bg_drain();
    auto it = std::upper_bound(guest_pools.begin(), guest_pools.end(), guest, [](uint64_t a, const GuestPool& p) { return a < p.lo; });
    if (it == guest_pools.begin()) return false;
    --it;
    if (guest < it->lo || guest >= it->hi || size > it->hi - guest) return false;
    const uint64_t chunk = import_chunk();
    if (!chunk || guest + size > UINT64_MAX - chunk) return false;
    if (auto c = imported.upper_bound(guest); c != imported.begin() && std::prev(c)->second.lo <= guest &&
                                              std::prev(c)->second.hi >= guest + size)
        return false;   // (covered)
    const uint64_t lo = guest / chunk * chunk, hi = (guest + size + chunk - 1) / chunk * chunk;
    for (auto o = imported.lower_bound(hi); o != imported.begin();) {   // (an import there: the synchronous path grows it)
        --o;
        if (o->second.hi <= lo) {
            if (o->second.hi + (1ull << 30) < lo) break;
            continue;
        }
        return false;
    }
    std::lock_guard lk(bg_mu);
    for (const uint64_t x : bg_inflight)
        if (x >= lo && x < hi) return true;   // (under way)
    if (bg_inflight.size() >= 16) return false;
    bg_start();
    bg_inflight.push_back(lo);
    bg_queue.push_back({lo, hi});
    bg_cv.notify_one();
    if (st_async_sent++ % 32 == 0)
        logf("vk: imports in the background: %lld sent, %lld bindings served from a copy meanwhile", (long long)st_async_sent,
             (long long)st_async_copies);
    return true;
}
} // namespace
uint64_t device_vram_bytes() {
    uint64_t vram = 0;
    for (uint32_t h = 0; h < dev.memory.memoryHeapCount; h++)
        if (dev.memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) vram = std::max(vram, (uint64_t)dev.memory.memoryHeaps[h].size);
    return vram;
}
bool preset_caps_memory() {
    static const uint64_t nominal_gb[] = {0, 4, 6, 8, 12, 16};
    if (opts.preset <= 0 || opts.preset > 5) return true;   // (Custom: what the file says)
    const uint64_t vram = device_vram_bytes();
    return !vram || vram <= ((nominal_gb[opts.preset] << 30) + (768ull << 20));
}
namespace {
uint64_t mirror_budget() {
    // (a quarter of the card, at most the 3 GB it was for all: on a 6 GB laptop those and the textures were past what the card has)
    static const uint64_t b = [] {
        if (const char* e = getenv("RX_VK_MIRROR_MB")) return (uint64_t)atoi(e) << 20;
        uint64_t vram = 0;
        for (uint32_t h = 0; h < dev.memory.memoryHeapCount; h++)
            if (dev.memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) vram = std::max(vram, (uint64_t)dev.memory.memoryHeaps[h].size);
        // (cards of 6 GB or less only: on 8 GB ones, a 3070 or a 4060, the game's memory read over PCIe instead left the GPU at a few
        // percent and the frame stuttering)
        if (vram && vram <= (6ull << 30) + (512ull << 20)) return vram / 8;
        return vram ? std::min<uint64_t>(vram / 4, 3072ull << 20) : (uint64_t)(3072ull << 20);
    }();
    // (the Insert menu's quality preset sets one, 256 MB at Minimum (4GB): it was never read, and an 8 GB card kept ~2 GB of
    // mirrors with the 4GB preset on, 2026-11-08)
    // (only on a card the preset is for: preset_caps_memory)
    if (opts.mirror_mb > 0 && !getenv("RX_VK_MIRROR_MB") && preset_caps_memory()) return (uint64_t)opts.mirror_mb << 20;
    return b;
}
uint64_t mirror_bytes = 0, mirror_epoch = 0;
bool mirror_upload = false;   // (a submission is being recorded: its upload command buffer takes the syncs)
std::vector<Pool*> mirror_wanted, mirror_synced;
struct WriteBack {
    Pool* pool;
    uint64_t lo, hi;   // offsets in the pool
    bool event = false;   // (an NVN event's word, written by a command buffer's signal: memory_bind_event_write)
};
std::vector<WriteBack> writebacks;
// (hookshot, 11-07) The counter reports the replay writes into guest memory on the CPU (vk_replay.cpp NOP_REPORT_COUNTER), by
// guest address: a mirrored pool's write-back copied the GPU's mirror over them at the end of the submission, the mirror's
// stale words over the replay's newer ones, and the next render condition read those: a lighting pass left out, the whole lit
// scene black for a frame (the one-frame dark flashes, every 15-30 frames; 17 in 40 s of Kokiri, 0 with mirrors off). They are
// left out of the write-back now. RX_VK_WB_REPORTS=1: written back with the rest, as before
std::mutex cpu_words_mu;
// (two sorted sets with no word in both, and the words noted since the last merge: those go into the recent set, which
// goes into the big one only once it is an eighth of its size. A std::set took a node a word and freed 65536 of them at
// once when it filled, ~4% of the replay thread in a Kokiri profile; one sorted vector with every write-back's new words
// merged in moved the whole of it each time, ~2.4% on a 3900X there (11-08): report words come at new addresses all the time)
std::vector<uint64_t> cpu_words, cpu_words_recent, cpu_words_new;
void sorted_merge(std::vector<uint64_t>& into, const uint64_t* b, const uint64_t* e) {
    const size_t mid = into.size();
    into.insert(into.end(), b, e);
    std::inplace_merge(into.begin(), into.begin() + (ptrdiff_t)mid, into.end());
}
void cpu_words_merge() {   // (holding cpu_words_mu)
    if (cpu_words_new.empty()) return;
    std::sort(cpu_words_new.begin(), cpu_words_new.end());
    cpu_words_new.erase(std::unique(cpu_words_new.begin(), cpu_words_new.end()), cpu_words_new.end());
    // (only the words not kept already: the same report words come every frame)
    size_t fresh = 0;
    for (const uint64_t w : cpu_words_new)
        if (!std::binary_search(cpu_words.begin(), cpu_words.end(), w) &&
            !std::binary_search(cpu_words_recent.begin(), cpu_words_recent.end(), w))
            cpu_words_new[fresh++] = w;
    if (fresh) sorted_merge(cpu_words_recent, cpu_words_new.data(), cpu_words_new.data() + fresh);
    if (cpu_words_recent.size() > std::max<size_t>(4096, cpu_words.size() / 8)) {
        sorted_merge(cpu_words, cpu_words_recent.data(), cpu_words_recent.data() + cpu_words_recent.size());
        cpu_words_recent.clear();
    }
    cpu_words_new.clear();
}
} // namespace
void memory_cpu_owned(uint64_t guest, uint64_t size) {
    static const bool off = getenv("RX_VK_WB_REPORTS") != nullptr;
    if (off || !guest) return;
    std::lock_guard lk(cpu_words_mu);
    if (cpu_words.size() + cpu_words_recent.size() + cpu_words_new.size() > 65536) {   // (as before: forgotten when it gets this big)
        cpu_words_merge();   // (which leaves cpu_words_new empty)
        if (cpu_words.size() + cpu_words_recent.size() > 65536) cpu_words.clear(), cpu_words_recent.clear();
    }
    for (uint64_t a = guest & ~7ull; a < guest + size; a += 8) cpu_words_new.push_back(a);
    // (the write itself, guest_cpu_write, has told the write-back diff's shadows)
}
namespace {
// (a write-back's runs with the replay's own words cut out of them: runs of p, offsets in the pool)
void cut_cpu_words(const Pool& p, std::vector<VkBufferCopy>& runs) {
    std::lock_guard lk(cpu_words_mu);
    cpu_words_merge();
    if (cpu_words.empty() && cpu_words_recent.empty()) return;
    std::vector<VkBufferCopy> out;
    for (const VkBufferCopy& r : runs) {
        uint64_t at = r.srcOffset;
        const uint64_t end = r.srcOffset + r.size;
        // A run can start inside an owned word: include that word when cutting the overlap.
        const uint64_t first = (p.lo + at) & ~7ull, last = p.lo + end;
        auto a = std::lower_bound(cpu_words.begin(), cpu_words.end(), first);
        auto b = std::lower_bound(cpu_words_recent.begin(), cpu_words_recent.end(), first);
        for (;;) {   // (both sets' words in the run, in order)
            const bool in_a = a != cpu_words.end() && *a < last, in_b = b != cpu_words_recent.end() && *b < last;
            if (!in_a && !in_b) break;
            const uint64_t w = (in_a && (!in_b || *a < *b) ? *a++ : *b++) - p.lo;
            if (w > at) out.push_back({at, at, w - at});
            at = std::max(at, w + 8);
        }
        if (end > at) out.push_back({at, at, end - at});
    }
    runs.swap(out);
}

// ---- a pool remade larger (a 16 MB chunk the game's next pool runs past): the old one leaves the table but stays
// alive, as this submission's bindings, its mirror syncs and its write-backs still use it. Erasing it left those
// lists pointing into a freed map node: the write-back copied with garbage handles and the device was lost (the
// daytime title scene, whose pools cross chunk ends). Retired pools are kept (a few, never reused). ----
// (Extracted from the table, not erased: the node, and the Pool in it, stay where they were, so the mirror's lists and
// a parallel replay thread's pool cache still point at a live one.)
std::deque<std::map<uint64_t, Pool>::node_type> retired_pools;

void retire_pool(std::map<uint64_t, Pool>::iterator it) {
    Pool* old = &it->second;
    shadow_free(*old);   // (its write-backs in this submission, if any, copied whole)
    guest_pages_set(old->lo, old->hi, 0);
    old->retired_value = std::max(old->last_value, mem_rec_value);
    std::erase(mirror_wanted, old);   // (the new pool gets the mirror)
    if (old->mstate == Pool::MIRROR_WANTED) old->mstate = Pool::MIRROR_NONE;
    // (EvilOOT, 11-07) what the GPU wrote into its mirror and nowhere else (the culling's bare table entries past their first
    // 64 bytes, stores through the page table) was lost here: the new pool's mirror is filled from guest memory. Kokiri's
    // foliage records, written by the GPU while the scene loaded and its pools were remade larger, came out zero: every
    // count 0, the trees' leaf clusters never drawn. RX_VK_NO_INHERIT=1: as before
    static const bool no_inherit = getenv("RX_VK_NO_INHERIT") != nullptr;
    if (old->mstate == Pool::MIRROR_ON && !no_inherit) old->inherit_pending = true, old->retired_epoch = mirror_epoch;
    pools_gen++;
    retired_pools.push_back(imported.extract(it));
    last_pool = nullptr;
    std::fill(std::begin(pool_cache), std::end(pool_cache), nullptr);
    static int logged;
    if (logged++ < 8 || retired_pools.size() % 64 == 0)
        logf("vk: pool %#llx remade larger (%zu retired)", (unsigned long long)old->lo, retired_pools.size());
}
int64_t st_sync_bytes, st_sync_copies, st_wb_bytes, st_mirrors;
int64_t st_wb_event_runs;   // (Triforce: write-back runs of NVN event words alone, not noted for the captures' wait)
int64_t st_sync_live_pages, st_sync_cap_pages, st_sync_old_pages;   // (mirror syncs' dirty pages: live / this submit's capture / an earlier one's)
std::unordered_map<uint64_t, int64_t> live_by_pool;
std::unordered_map<uint64_t, std::pair<int64_t, int>> direct_by_pool;   // (bindings the GPU reads in guest memory: count, state)
std::unordered_map<uint64_t, uint64_t> direct_bytes_by_pool;           // (and the bytes of those bindings: what the GPU may read over PCIe, a binding's whole range)
int64_t live_seen = 0;   // (pool -> pages its syncs read live, since the last report)

void transfer_barrier(VkCommandBuffer cmd) {
    full_barrier(cmd, "barrier (write-back or sync)");   // (the same barrier; one right after another is left out)
}

// Captured pages in a run are copied from one buffer to one mirror. Combining only contiguous source and
// destination bytes leaves gaps, overlaps and the order of the remaining regions unchanged.
void append_capture_copy(std::vector<VkBufferCopy>& runs, const VkBufferCopy& next) {
    if (!runs.empty()) {
        VkBufferCopy& last = runs.back();
        if (last.size && next.size && last.size <= UINT64_MAX - last.srcOffset &&
            last.size <= UINT64_MAX - last.dstOffset && next.size <= UINT64_MAX - last.size &&
            next.size <= UINT64_MAX - next.srcOffset && next.size <= UINT64_MAX - next.dstOffset &&
            last.srcOffset + last.size == next.srcOffset && last.dstOffset + last.size == next.dstOffset) {
            last.size += next.size;
            return;
        }
    }
    runs.push_back(next);
}

/// The pages of the pool the CPU wrote since its mirror's sync, copied in (the upload command buffer)
std::vector<uint64_t> demote_wanted;
thread_local bool tl_from_compute = false;   // (Evil Hookshot: pools to leave the mirrors at the next submission's start)

// ---- (Triforce, 2026-11-11) mirrors in pieces ----
// A mirror was a device-local copy of its whole import, and an import is whole 16 MB chunks of the game's address space, merged
// with its neighbours whenever a range crosses a chunk's end (a texture's storage read for its upload does). The bindings reach
// little of that: at the Market after a tour of ten areas, 70 mirrors held 2,992 MB (their budget) of which 427 MB had been
// bound, in 64 KB steps (one of 256 MB: 384 KB bound; RX_VK_LOG_MIRROR_USE). On a card of 8 GB that is the VRAM the textures
// and the write-back's shadows then lacked: the Market ran at 40 fps there against 54 with 1.5 GB of mirrors (the shadows given
// back, the texture budget squeezed to half of what the scene samples), and a 12 GB card went past its allowance in a tour.
// Now a mirror is a sparse buffer of the pool's size, with memory bound only under the pieces (RX_VK_MIRROR_PIECE_KB, 256) a
// binding has reached: bound, filled as a new mirror is (guest memory, then the captures' pages over it) and entered in the
// page table at that binding, in its recording's uploads. Offsets are the pool's, as before. What no binding has reached stays
// as an unmirrored pool's memory is: the page table points at the import, the mirror's syncs skip it, and a binding that cannot
// have its pieces (the budget, the driver's allowance, a parallel replay's segment) is given the import for that submission.
// RX_VK_MIRROR_PIECES=0: whole mirrors, as before; =1: in pieces whatever the trial says. On any card with sparse residency
// buffers: played on NVIDIA's, on trial on the others (memory_trial_on, above: off by itself where a session with it loses the
// GPU or two end in a crash).
bool mirror_pieces_on() {
    static const bool on = [] {
        const char* e = getenv("RX_VK_MIRROR_PIECES");
        const bool can = dev.sparse_buffers && dev.features.sparseResidencyBuffer;
        const bool v = can && (e ? atoi(e) != 0 : memory_trial_on());
        logf("vk: mirrors in pieces: %s%s", v ? "on" : "off", !can ? " (no sparse residency buffers)" : e ? " (RX_VK_MIRROR_PIECES)" : "");
        return v;
    }();
    return on;
}
struct PieceWanted { uint64_t lo, off, size; };
std::vector<PieceWanted> piece_wanted;   // (ranges bound in a parallel replay's segment: made at the next submission's start; pools_mu)
struct PieceBind { VkBuffer buffer; VkSparseMemoryBind bind; };
std::mutex piece_binds_mu;
std::vector<PieceBind> piece_binds;   // (made in the recording being made: bound before it is submitted, memory_mirror_binds_flush)
int64_t st_piece_binds = 0, st_piece_flushes = 0, st_piece_direct = 0;
double st_piece_bind_ms = 0;
inline bool piece_resident(const Pool* p, uint64_t i) {
    return (__atomic_load_n(&p->res_bits[i >> 6], __ATOMIC_RELAXED) >> (i & 63)) & 1;
}
/// (Triforce) How a pool the CPU and the GPU both write leaves a mirror in pieces (demote_trials), RX_VK_DEMOTE_PIECES:
/// 0: whole, as a whole mirror does (and as every build before 2026-11-11 did).
/// 1: its pieces holding such pages are tried and leave, the pool stays mirrored (the first version: a player saw a flicker
///    that was not there before; many more pieces left than pools ever had, their trials' cost being small).
/// 2: the pool is tried whole as in 0, so the same pools pass; one that passes is mirrored again without its pieces holding
///    such pages, and pieces found later leave it at once.
int demote_pieces_mode() {
    static const int m = getenv("RX_VK_DEMOTE_PIECES") ? std::clamp(atoi(getenv("RX_VK_DEMOTE_PIECES")), 0, 2) : 1;
    return m;
}
/// A piece of the pool's [off, off + size) is left in guest memory for good (Pool::direct_bits): the binding reads the import
inline bool pieces_direct(const Pool* p, uint64_t off, uint64_t size) {
    if (!p->mpieces || !size || p->direct_bits.empty()) return false;
    for (uint64_t i = off >> p->piece_shift, e = (off + size - 1) >> p->piece_shift; i <= e; i++) {
        const uint64_t w = __atomic_load_n(&p->direct_bits[i >> 6], __ATOMIC_RELAXED);
        if (!w) {   // (a clear word: on to the next)
            i |= 63;
            continue;
        }
        if ((w >> (i & 63)) & 1) return true;
    }
    return false;
}
/// (pools_mu held) The pieces in bits out of the mirror (direct) or back among those it may hold: a piece taken out loses its
/// place in the page table (the import's again) and its resident bit; its memory stays with the mirror until that goes
void pieces_set_direct(Pool& p, const std::vector<uint64_t>& bits, bool direct) {
    const uint64_t n = p.hi - p.lo, piece = 1ull << p.piece_shift;
    for (size_t w = 0; w < bits.size() && w < p.direct_bits.size(); w++)
        for (uint64_t m = bits[w]; m; m &= m - 1) {
            const uint64_t i = (uint64_t)w * 64 + (uint64_t)std::countr_zero(m);
            if (!direct) {
                __atomic_fetch_and(&p.direct_bits[w], ~(1ull << (i & 63)), __ATOMIC_RELAXED);
                continue;
            }
            __atomic_fetch_or(&p.direct_bits[w], 1ull << (i & 63), __ATOMIC_RELAXED);
            if (!piece_resident(&p, i)) continue;
            __atomic_fetch_and(&p.res_bits[w], ~(1ull << (i & 63)), __ATOMIC_RELAXED);
            const uint64_t a = i * piece, b = std::min((i + 1) * piece, n);
            if (a < b) guest_pages_set(p.lo + a, p.lo + b, p.address + a);
        }
}
/// Every piece of the pool's [off, off + size) has its memory (true for a whole mirror, and for nothing)
inline bool pieces_resident(const Pool* p, uint64_t off, uint64_t size) {
    if (!p->mpieces || !size) return true;
    for (uint64_t i = off >> p->piece_shift, e = (off + size - 1) >> p->piece_shift; i <= e; i++) {
        if ((i & 63) == 0 && i + 63 <= e && __atomic_load_n(&p->res_bits[i >> 6], __ATOMIC_RELAXED) == ~0ull) {
            i += 63;   // (a whole word of them)
            continue;
        }
        if (!piece_resident(p, i)) return false;
    }
    return true;
}
/// Copies into the pool's mirror (by dstOffset; by_src: out of it, by srcOffset), cut down to its resident pieces
void clip_resident(const Pool& p, std::vector<VkBufferCopy>& copies, bool by_src = false) {
    if (!p.mpieces || copies.empty()) return;
    static thread_local std::vector<VkBufferCopy> out;
    out.clear();
    const uint64_t piece = 1ull << p.piece_shift;
    for (const VkBufferCopy& r : copies) {
        uint64_t run = UINT64_MAX;   // (the start of the resident run open now, an offset in the mirror)
        const uint64_t base = by_src ? r.srcOffset : r.dstOffset, end = base + r.size;
        const auto emit = [&](uint64_t a, uint64_t b) { out.push_back({r.srcOffset + (a - base), r.dstOffset + (a - base), b - a}); };
        for (uint64_t at = base; at < end;) {
            const uint64_t next = std::min(end, (at / piece + 1) * piece);
            const bool res = piece_resident(&p, at >> p.piece_shift);
            if (res && run == UINT64_MAX) run = at;
            if (!res && run != UINT64_MAX) emit(run, at), run = UINT64_MAX;
            at = next;
        }
        if (run != UINT64_MAX) emit(run, end);
    }
    copies.assign(out.begin(), out.end());
}
/// The pages of the pool's [a, b) (offsets) that a capture holds as they were at their submit, copied over the mirror there
/// (after a copy of guest memory, which is the game's frame ahead): mirror_make's, for a part
void mirror_captures_over(Pool* p, uint64_t a, uint64_t b) {
    std::map<VkBuffer, std::vector<VkBufferCopy>> over;
    for (uint64_t page = (p->lo + a) >> kPageShift; page < (p->lo + b + 4095) >> kPageShift; page++) {
        const CapEntry e = cap_entry(page);
        if (!e.gen || e.gen != page_gen(page)) continue;
        {   // (Triforce) not a copy taken while the GPU was still writing the page and never taken again (take_capture: the
            // pool had no mirror then): guest memory, copied whole above on the GPU's timeline, has the GPU's words
            auto ls = late_skipped_gen.find((uint32_t)page);
            if (ls != late_skipped_gen.end() && ls->second == e.gen) continue;
            auto lp = late_partial.find((uint32_t)page);   // (nor one synced in part: the same)
            if (lp != late_partial.end() && lp->second.gen == e.gen) continue;
        }
        auto cb = cap_bufs.find(e.gen);
        if (cb == cap_bufs.end()) continue;
        const uint64_t ga = std::max(page << kPageShift, p->lo + a), gb = std::min((page + 1) << kPageShift, p->lo + b);
        if (ga >= gb) continue;
        over[cb->second.buffer].push_back({(uint64_t)e.slot * 4096 + (ga - (page << kPageShift)), ga - p->lo, gb - ga});
        cb->second.last_use = std::max(cb->second.last_use, mem_rec_value);
    }
    if (over.empty()) return;
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};   // (after the copy of guest memory)
    mb.srcStageMask = mb.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(upload_command_buffer(), &di);
    for (const auto& [buf, rs] : over)
        vkCmdCopyBuffer(upload_command_buffer(), buf, p->mirror, (uint32_t)rs.size(), rs.data());
}
/// The pieces of the pool's [off, off + size) that have no memory yet: given it (bound at the recording's end), copied from
/// guest memory in the recording's uploads and entered in the page table; with captures, the captures' pages over them too
/// (not when the caller has copies of its own to put between: mirror_make's inherited parts). False when the budget or the
/// driver has no room: nothing of that run is made, the caller binds the import. (pools_mu held)
bool mirror_pieces_make(Pool& p, uint64_t off, uint64_t size, bool captures, std::vector<std::pair<uint64_t, uint64_t>>* made = nullptr) {
    if (!p.mpieces || !size) return true;
    if (pieces_direct(&p, off, size)) return false;   // (a piece left in guest memory for good: the whole binding reads the import)
    const uint64_t n = p.hi - p.lo, piece = 1ull << p.piece_shift;
    const uint64_t first = off >> p.piece_shift, last = std::min(off + size - 1, n - 1) >> p.piece_shift;
    static thread_local std::vector<std::pair<uint64_t, uint64_t>> runs;
    runs.clear();
    for (uint64_t i = first; i <= last; i++) {
        if (piece_resident(&p, i)) continue;
        if (!runs.empty() && runs.back().second == i * piece) runs.back().second = (i + 1) * piece;
        else runs.push_back({i * piece, (i + 1) * piece});
    }
    if (runs.empty()) return true;
    uint64_t want = 0;
    for (const auto& [a, b] : runs) want += std::min(b, p.mirror_size) - a;
    if (mirror_bytes + want > mirror_budget()) return false;
    std::vector<VkBufferCopy> copies;
    for (const auto& [a, b] : runs) {
        const uint64_t bind_end = std::min(b, p.mirror_size), copy_end = std::min(b, n);
        VmaAllocation alloc = nullptr;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        uint64_t mem_off = 0;
        if (!vma_piece_alloc(bind_end - a, p.piece_align, p.piece_types, alloc, mem, mem_off)) {
            static int told;
            if (told++ < 8)
                logf("vk: pool %#llx: no device memory for %llu KB of its mirror: those bindings read guest memory",
                     (unsigned long long)p.lo, (unsigned long long)((bind_end - a) >> 10));
            return false;   // (the runs before it stay: they are bound and filled)
        }
        p.res_allocs.push_back(alloc);
        {
            std::lock_guard lk(piece_binds_mu);
            piece_binds.push_back({p.mirror, {a, bind_end - a, mem, mem_off, 0}});
        }
        for (uint64_t i = a >> p.piece_shift; i < (b >> p.piece_shift); i++)
            __atomic_fetch_or(&p.res_bits[i >> 6], 1ull << (i & 63), __ATOMIC_RELAXED);
        p.res_bytes += bind_end - a, mirror_bytes += bind_end - a;
        copies.push_back({a, a, copy_end - a});
        guest_pages_set(p.lo + a, p.lo + copy_end, p.mirror_address + a);
        if (made) made->push_back({a, copy_end});
        st_piece_binds++;
    }
    {
        GprofSpan gp(upload_command_buffer(), "mpiece copy (a part of a pool -> its mirror's new pieces)");
        gp.items = (int)copies.size();
        for (const VkBufferCopy& c : copies) gp.bytes += c.size, st_sync_bytes += (int64_t)c.size;
        vkCmdCopyBuffer(upload_command_buffer(), p.buffer, p.mirror, (uint32_t)copies.size(), copies.data());
    }
    if (captures)
        for (const VkBufferCopy& c : copies) mirror_captures_over(&p, c.dstOffset, c.dstOffset + c.size);
    return true;
}

void mirror_sync(Pool& p) {
    {   // (Evil Hookshot: the pool's bindings since its last sync, averaged: a pool the GPU reads all the time stays mirrored)
        const uint32_t b = __atomic_load_n(&p.binds, __ATOMIC_RELAXED);
        p.bind_rate = p.bind_rate * 0.8f + (float)(b - p.binds_at_sync) * 0.2f;
        p.binds_at_sync = b;
    }
    p.synced_epoch = mirror_epoch;
    p.sync_seg = texture_segment();
    mirror_synced.push_back(&p);
    std::vector<VkBufferCopy> runs;   // (from guest memory)
    std::map<VkBuffer, std::vector<VkBufferCopy>> cap_runs;   // (from the captures' copies)
    uint64_t run_lo = 0, run_hi = 0;
    bool conflict = false;   // (Evil Hookshot: a dirty page the GPU wrote, see Pool::gpu_bits)
    for (uint64_t page = p.lo >> kPageShift; page < (p.hi + 4095) >> kPageShift;) {
        if (page % kRegionPages == 0 && region_gen[page / kRegionPages] <= p.synced_gen) {
            page += kRegionPages;   // (a clean 2 MB region)
            continue;
        }
        const uint32_t pg = page_gen(page);
        const bool dirty = pg > p.synced_gen;
        const bool by_piece = p.mpieces && demote_pieces_mode() != 0;
        if (dirty && (!conflict || by_piece) && !p.gpu_bits.empty()) {
            const uint64_t q = page - (p.lo >> kPageShift);
            if ((q >> 6) < p.gpu_bits.size() && (p.gpu_bits[q >> 6] >> (q & 63) & 1)) {
                if (!by_piece) conflict = true;
                else if (const uint64_t i = (q << kPageShift) >> p.piece_shift; piece_resident(&p, i)) {
                    // (Triforce: in pieces, the piece it is in: only those leave the mirror, demote_trials)
                    conflict = true;
                    if (p.conf_bits.size() != p.res_bits.size()) p.conf_bits.assign(p.res_bits.size(), 0);
                    p.conf_bits[i >> 6] |= 1ull << (i & 63);
                }
            }
        }
        if (dirty && gpu_page(page)) {   // (diagnostic: see memory_stamp_gpu)
            st_sync_gpu_pages++;
            const CapEntry e0 = cap_entry(page);
            const bool live = !(e0.gen && e0.gen == pg && cap_bufs.count(e0.gen));
            st_sync_gpu_live += live;
            static int logged;
            if (logged++ < 40)
                logf("vk: a mirror sync copies the CPU's version of page %#llx over the GPU's (pool %#llx, %s, page gen %u, "
                     "pool synced at %u, gen now %u)", (unsigned long long)(page << kPageShift), (unsigned long long)p.lo,
                     live ? "live" : "from a capture", pg, (unsigned)p.synced_gen, gen_now);
        }
        if (dirty && protect_bare_on() && prot_page_since(page, p.synced_gen)) {   // (the GPU's records kept: see prot_pages)
            st_prot_kept++;
            page++;
            continue;
        }
        if (dirty && !sync_live()) {   // (as a capture took it, if the page was not written since)
            const CapEntry e = cap_entry(page);
            if (e.gen && e.gen == pg) {
                auto b = cap_bufs.find(e.gen);
                if (b != cap_bufs.end()) {
                    const uint64_t ga = std::max(page << kPageShift, p.lo), gb = std::min((page + 1) << kPageShift, p.hi);
                    // (Triforce) a late page synced in part: what no submission on the GPU was writing back when it was taken
                    const LatePartial* part = nullptr;
                    if (!late_partial.empty()) {
                        auto lp = late_partial.find((uint32_t)page);
                        if (lp != late_partial.end() && lp->second.gen == e.gen) part = &lp->second;
                    }
                    if (part) {
                        const uint64_t base = page << kPageShift;
                        for (int k = 0; k < part->n; k++) {
                            const uint64_t a = std::max(base + part->keep[k][0], ga), z = std::min(base + part->keep[k][1], gb);
                            if (z > a) append_capture_copy(cap_runs[b->second.buffer], {(uint64_t)e.slot * 4096 + (a - base), a - p.lo, z - a});
                        }
                    } else
                    append_capture_copy(cap_runs[b->second.buffer],
                        {(uint64_t)e.slot * 4096 + (ga - (page << kPageShift)), ga - p.lo, gb - ga});
                    b->second.last_use = std::max(b->second.last_use, cur_cap_value);
                    (e.gen == cur_cap_gen ? st_sync_cap_pages : st_sync_old_pages)++;
                    page++;
                    continue;
                }
            }
        }
        if (dirty) {
            st_sync_live_pages++;   // (dirty, not in this submit's capture: guest memory as it is now)
            live_by_pool[p.lo]++;
            {   // (RX_VK_LOG_LIVE=1: why, 300 lines: the page's generation, its entry's, the newest capture's)
                static const bool log_live = getenv("RX_VK_LOG_LIVE") != nullptr;
                static int logged;
                if (log_live && logged < 300 && (++live_seen % 97) == 0) {
                    logged++;
                    const CapEntry e = cap_entry(page);
                    logf("vk: live page %#llx: gen %u, entry gen %u (buffer %s), current capture %u, pool synced at %u, gen now %u",
                         (unsigned long long)(page << kPageShift), pg, e.gen,
                         e.gen ? (cap_bufs.count(e.gen) ? "kept" : "gone") : "-", cur_cap_gen, (unsigned)p.synced_gen, gen_now);
                }
            }
            const uint64_t a = std::max(page << kPageShift, p.lo) - p.lo, b = std::min((page + 1) << kPageShift, p.hi) - p.lo;
            if (run_hi == a && run_hi) run_hi = b;
            else {
                if (run_hi) runs.push_back({run_lo, run_lo, run_hi - run_lo});
                run_lo = a, run_hi = b;
            }
        }
        page++;
    }
    if (run_hi) runs.push_back({run_lo, run_lo, run_hi - run_lo});
    if (conflict && p.direct_auto && demote_pieces_mode() == 2) {
        // (Triforce: a pool whose trial left it out of the mirrors (demote_trials): its pieces with such pages leave at once, as
        // the whole pool had before)
        pieces_set_direct(p, p.conf_bits, true);
        std::fill(p.conf_bits.begin(), p.conf_bits.end(), 0ull);
        pools_gen++;
        conflict = false;
    }
    if (conflict) {   // (Evil Hookshot: 8 syncs that put the CPU's pages over the GPU's: the pool leaves the mirrors, see demote_pools)
        // (Candidates: pools a compute program writes (a GPU simulation the CPU reads back: the particles; pools written by copies
        // or draws only cost GPU time out of the mirrors, ~1 ms each, and gave nothing), up to 64 MB (RX_VK_DEMOTE_MB). Whether one stays out of the mirrors is measured, not guessed:
        // demote_trials puts it in guest memory and compares the GPU's time a frame; a pool every pixel reads (the scene's light
        // constants: 40 -> 8 fps) or the indirect arguments (-> 29) go back. Bindings a sync did not tell them apart.
        // RX_VK_NO_DEMOTE=1: none, as before)
        static const uint64_t cap_mb = getenv("RX_VK_DEMOTE_MB") ? strtoull(getenv("RX_VK_DEMOTE_MB"), nullptr, 10) : 64;
        static const bool off = getenv("RX_VK_NO_DEMOTE") != nullptr;
        ++p.conflict_syncs;
        static const bool log_c = getenv("RX_VK_LOG_DEMOTE") != nullptr;
        if (log_c && p.conflict_syncs % 50 == 8)
            logf("vk: pool %#llx (%llu MB): CPU over GPU pages in %u syncs, %.1f bindings a sync", (unsigned long long)p.lo,
                 (unsigned long long)((p.hi - p.lo) >> 20), p.conflict_syncs, p.bind_rate);
        // (Triforce: RX_VK_DEMOTE_PIECES=1: a mirror in pieces of any size: only the pieces with such pages leave it)
        if (p.conflict_syncs >= 8 && !p.demote_queued && !off && ((p.mpieces && demote_pieces_mode() == 1) || p.hi - p.lo <= (cap_mb << 20)) &&
            !p.demote_blocked && p.compute_writes) {
            p.demote_queued = true;
            demote_wanted.push_back(p.lo);
        }
    }
    for (auto& [buf, rs] : cap_runs) {
        clip_resident(p, rs);   // (Triforce: a mirror in pieces takes the pages of its resident pieces only)
        if (rs.empty()) continue;
        GprofSpan gp(upload_command_buffer(), "msync copies (CPU pages from a capture -> mirror)");
        gp.items = (int)rs.size();
        vkCmdCopyBuffer(upload_command_buffer(), buf, p.mirror, (uint32_t)rs.size(), rs.data());
        for (const VkBufferCopy& r : rs) st_sync_bytes += (int64_t)r.size, gp.bytes += r.size;
        st_sync_copies += (int64_t)rs.size();
    }
    clip_resident(p, runs);
    if (runs.empty()) return;
    checkpoint_mark(upload_command_buffer(), p.lo, 'm');
    GprofSpan gp(upload_command_buffer(), "msync copies (CPU pages live from guest memory -> mirror)");
    gp.items = (int)runs.size();
    vkCmdCopyBuffer(upload_command_buffer(), p.buffer, p.mirror, (uint32_t)runs.size(), runs.data());
    for (const VkBufferCopy& r : runs) st_sync_bytes += (int64_t)r.size, gp.bytes += r.size;
    st_sync_copies += (int64_t)runs.size();
}

} // namespace

void memory_bind_from_compute(bool on) { tl_from_compute = on; }

// ---- (Triforce) An NVN event's word, written on the GPU's timeline by a command buffer's signal (vk_replay.cpp NOP_SIGNAL_EVENT):
// its write-back is not one a capture waits for. wb_pages notes every page a submission writes back, and a capture whose pages
// the CPU wrote waits (take_capture, "late pages") for the last submission still on the GPU that writes such a page back, so that
// the page's mirror sync does not put the GPU's older words back over its newer ones (OoT's exposure meter: capture_make). OoT
// keeps all its queue-ordering events in one page (0x100d6c8000 in build e0f3ccfa: +0, +8, +0x14, ...), a command buffer of
// every frame signals one, and the game's CPU writes that page too: at the Castle Town save every late page of every capture was
// that one page, and the game's submitting thread waited for the GPU to finish the frame before, 1.8 ms a frame at 1440p and
// 4.6 ms at 2160p on an RTX 4070 SUPER (58.8 -> 67.3 and 49.1 -> 63.3 fps with RX_VK_CAPTURE_WAIT=0, which flickers: it drops
// the exposure meter's wait too). An event's word in the mirror is read by nothing: waits are ordered on the CPU from the
// signals' values (ev_values), the CPU reads the word in guest memory (nvnEventGetValue), and a write-back copies the ranges
// written in its own submission only, so a mirror word a page sync made stale is never copied out; the next signal writes it
// before its own write-back. RX_VK_EVENT_LATE=1: noted, as before ----
thread_local bool tl_event_write = false;
void memory_bind_event_write(bool on) {
    // (off unless RX_VK_EVENT_LATE=0: alone it changed nothing that could be measured, the page's other late pages still wait
    // (58.5 fps either way, no flicker either way, 2026-11-11), so the build keeps the behaviour that was played)
    static const bool as_before = [] {
        const bool v = !(getenv("RX_VK_EVENT_LATE") && atoi(getenv("RX_VK_EVENT_LATE")) == 0);
        if (!v) logf("vk: the write-backs of event words (a command buffer's signal) are not waited for by captures (RX_VK_EVENT_LATE=0)");
        return v;
    }();
    tl_event_write = on && !as_before;
}

// ---- (from PR 430, YunaraDebugs) Readback pages: GPU results the CPU reads (invalidate) and then writes, as OoT's exposure meter (a
// sum the GPU adds into each frame; the CPU reads it and stores 0 back). On the console the GPU adds into that memory as it is when
// its work runs, the CPU's 0 included. Here it adds into the pool's mirror, which takes the CPU's 0 only at a mirror sync of a submit
// made after it: one made just before (the game's threads overlap) added onto the old sum, the CPU read 2x the next frame and 0 the
// one after, and the exposure stepped (the light dimming or flaring for a frame in Kokiri). So each submission first copies those
// pages from guest memory into the mirror, on the GPU as its work starts, last of the uploads: what the CPU wrote by then is what the
// GPU adds to. RX_VK_NO_RB_REFRESH=1: off ----
// (hookshot, 11-09) A parallel replay's recordings share one submission (mirror_epoch) and its one write-back at its end: the part
// before the segments, each segment and the recording after them (vk_replay.cpp replay_handle). A refresh in a later one copied the
// guest's page, which has none of the submission's GPU writes, over what the recordings ahead of it had written in the mirror: OoT's
// exposure sum came back ~0 and every frame was too bright (RX_VK_PARALLEL=4). So a recording that joined an open submission
// refreshes a page only when every recording ahead of it in the queue is made and none of them may have written the page: the copy
// then brings the CPU's writes and wipes nothing (and a page first read in the middle of a submission is refreshed ahead of its first
// use, unless the recordings up to that use were all made before it was read). What they may have written are the GPU write
// hazards: every range bound for writing in the mirror, and the ranges a program may write that are not copied back (a bare table
// entry's guessed range past its first 64 bytes, the pointer table's, a wide binding: vk_draw.cpp, memory_note_gpu_write). A
// recording that opens its submission, or comes after its write-back (frame generation's), refreshes every page as before: serial
// replay records the same copies. With RX_VK_FOLLOW_STORES (stores through the page table, bound nowhere) a joining one refreshes
// none. RX_VK_RB_REFRESH_EACH=1: in every recording, as before
namespace {
std::unordered_map<uint32_t, uint64_t> rb_pages;   // (guest page -> mirror_epoch it was last read at)
std::atomic<int64_t> st_rb_refresh{0}, st_rb_held{0};
// (the open submission the recording this thread is making joined; 0: none: it opens one, or comes after one's end)
thread_local uint64_t rb_joined = 0;
// (the GPU write hazards of submission rb_hazards_epoch, by guest address, with their recording's place in the queue
// (texture_segment); kept past its write-back, for the recording that ends it)
struct RbHazard {
    uint64_t lo, hi, seg;
};
std::vector<RbHazard> rb_hazards;
uint64_t rb_hazards_epoch = 0;
// (a parallel replay's segments: the submission, the first one's order (texture_segment) and how many; the ones made, by order)
struct RbSegments {
    uint64_t epoch = 0, base = 0, n = 0, made = 0;
} rb_segs;
void rb_hazard_locked(uint64_t lo, uint64_t hi) {   // (pools_mu held)
    if (rb_hazards_epoch != mirror_epoch) rb_hazards.clear(), rb_hazards_epoch = mirror_epoch;
    rb_hazards.push_back({lo, hi, texture_segment()});
}
}
void memory_note_readback(uint64_t guest, uint64_t size) {
    static const bool off = getenv("RX_VK_NO_RB_REFRESH") != nullptr;
    if (off || !size || size > 0x4000) return;   // (small results only: meters and counters)
    std::lock_guard lk(pools_mu);
    for (uint64_t pg = guest >> kPageShift; pg <= (guest + size - 1) >> kPageShift; pg++) rb_pages[(uint32_t)pg] = mirror_epoch;
}
void memory_note_gpu_write(uint64_t gpu, uint64_t size) {
    if (!size) return;
    uint64_t guest = gpu;
    if (gpu >= (1ull << 40)) {   // (as gpu_buffer_bind)
        uint64_t avail = 0;
        if (!host->gpu_ptr(gpu, &avail) || avail < size) return;
        guest = host->gpu_guest(gpu);
    }
    std::lock_guard lk(pools_mu);
    rb_hazard_locked(guest, guest + size);
}
void memory_rb_recording_begun() {
    std::lock_guard lk(pools_mu);
    rb_joined = mirror_upload ? mirror_epoch : 0;
}
void memory_rb_segments_begin(uint64_t base, uint64_t n) {
    std::lock_guard lk(pools_mu);
    rb_segs = {mirror_epoch, base, n, 0};
}
/// Records the refresh copies into the submission's upload command buffer; true when it recorded any
bool memory_rb_refresh(VkCommandBuffer up) {
    std::lock_guard lk(pools_mu);
    static const bool each = getenv("RX_VK_RB_REFRESH_EACH") != nullptr;
    static const bool follow_stores = getenv("RX_VK_FOLLOW_STORES") != nullptr;
    const bool joined = !each && rb_joined && rb_joined == mirror_epoch;
    const uint64_t seg = texture_segment();
    bool ahead_made = !follow_stores;   // (every recording ahead of this one in the queue made, and what they may write known)
    if (joined && rb_segs.epoch == mirror_epoch && seg >= rb_segs.base && seg - rb_segs.base < rb_segs.n) {
        const uint64_t k = seg - rb_segs.base;   // (a segment's: the ones before it made, and this one now)
        const uint64_t below = k < 64 ? (1ull << k) - 1 : ~0ull;
        ahead_made = ahead_made && k < 64 && (rb_segs.made & below) == below;
        if (k < 64) rb_segs.made |= 1ull << k;
    }
    if (rb_pages.empty() || !up) return false;
    auto written_ahead = [&](uint64_t lo, uint64_t hi) {   // (may be, by a recording ahead of this one in its submission)
        if (rb_hazards_epoch != mirror_epoch) return false;
        for (const RbHazard& h : rb_hazards)
            if (h.seg < seg && h.lo < hi && lo < h.hi) return true;
        return false;
    };
    std::map<Pool*, std::vector<VkBufferCopy>> by_pool;
    for (auto it = rb_pages.begin(); it != rb_pages.end();) {
        if (it->second + 600 < mirror_epoch) { it = rb_pages.erase(it); continue; }   // (not read for a while)
        const uint64_t g = (uint64_t)it->first << kPageShift;
        ++it;
        auto pit = imported.upper_bound(g);
        if (pit == imported.begin()) continue;
        --pit;
        Pool& p = pit->second;
        if (g < p.lo || g >= p.hi || p.mstate != Pool::MIRROR_ON || !p.mirror) continue;
        const uint64_t a = g - p.lo, b = std::min(g + 4096, p.hi) - p.lo;
        if (joined && (!ahead_made || written_ahead(p.lo + a, p.lo + b))) {   // (not here: see above)
            st_rb_held++;
            continue;
        }
        by_pool[&p].push_back({a, a, b - a});
    }
    if (by_pool.empty()) return false;
    full_barrier(up);   // (after the older submissions' write-backs into guest memory and this one's other uploads)
    GprofSpan gp(up, "rbrefresh copies (readback pages guest -> mirror)");
    gp.items = 0;
    for (auto& [pp, rs] : by_pool) {
        st_rb_refresh += (int64_t)rs.size();
        // (hookshot) pages next to each other as one copy (~530 a frame in Hyrule Field, a copy a page)
        std::sort(rs.begin(), rs.end(), [](const VkBufferCopy& x, const VkBufferCopy& y) { return x.srcOffset < y.srcOffset; });
        size_t k = 0;
        for (size_t i = 1; i < rs.size(); i++)
            if (rs[k].srcOffset + rs[k].size == rs[i].srcOffset) rs[k].size += rs[i].size;
            else rs[++k] = rs[i];
        rs.resize(k + 1);
        clip_resident(*pp, rs);   // (Triforce: a page of a piece with no memory is read in guest memory itself)
        if (rs.empty()) continue;
        vkCmdCopyBuffer(up, pp->buffer, pp->mirror, (uint32_t)rs.size(), rs.data());
        gp.items += (int)rs.size();
        for (const VkBufferCopy& r : rs) gp.bytes += r.size;
    }
    static std::atomic<int64_t> n{0};
    if (++n % 3000 == 1)
        logf("vk: readback pages refreshed from guest memory: %lld pages so far (%lld held: may be written ahead in their submission)",
             (long long)st_rb_refresh.load(), (long long)st_rb_held.load());
    return true;
}

BufferRef gpu_buffer(uint64_t gpu, uint64_t size) {
    uint64_t avail = 0;
    if (!host->gpu_ptr(gpu, &avail) || avail < size) return {};
    const uint64_t guest = host->gpu_guest(gpu);
    std::lock_guard lk(pools_mu);
    Pool* p = pool_of(guest, size);
    if (!p) return {};
    if (p->last_value < mem_rec_value) p->last_value = mem_rec_value;   // (in use: not given back under it, imports_give_back)
    return {p->buffer, guest - p->lo, size, p->address + (guest - p->lo)};
}

void memory_pool_created(uint64_t gpu, uint64_t size) {
    // (RX_VK_BG_IMPORT=1 only: imported eagerly, every pool the game made, most never used by the GPU, the thread's
    // page locking stalled the game's threads (3-7 fps); chunked imports at first use are the default)
    static const bool on = getenv("RX_VK_BG_IMPORT") != nullptr;
    if (!on || !size || gpu >= (1ull << 40) || !dev.external_memory_host) return;   // (virtual pools are not imported)
    const uint64_t lo = gpu & ~(align - 1), hi = (gpu + size + align - 1) & ~(align - 1);
    std::lock_guard lk(bg_mu);
    bg_start();
    if (std::find(bg_inflight.begin(), bg_inflight.end(), lo) != bg_inflight.end()) return;
    bg_inflight.push_back(lo);
    bg_queue.push_back({lo, hi});
    bg_cv.notify_one();
}

int memory_guest_imported(uint64_t lo, uint64_t hi) {
    std::lock_guard lk(pools_mu);
    auto it = imported.upper_bound(hi);   // (pools by lo: those starting below hi, the last ones checked first)
    while (it != imported.begin()) {
        --it;
        if (it->second.hi > lo) return 1;
        if (it->second.hi + (1ull << 30) < lo) break;   // (imports are at most 1 GB: none further down reaches lo)
    }
    for (const auto& nh : retired_pools)
        if (nh.mapped().memory && nh.mapped().lo < hi && lo < nh.mapped().hi) return 1;
    return 0;
}

bool mirror_make(Pool& pool);   // (below)
/// The mirrored pools, for the submit-time capture (memory_capture_submit) (pools_mu held)
void publish_cap_ranges() {
    std::vector<std::pair<uint64_t, uint64_t>> v;
    for (const auto& [lo, pool] : imported)
        if (pool.mstate == Pool::MIRROR_ON || pool.was_mirrored) v.push_back({pool.lo, pool.hi});
    std::lock_guard lk(cap_ranges_mu);
    cap_ranges.swap(v);
    cap_ranges_ver++;
}
// (EvilOOT, 11-07) the game's own memory pool holding a GPU address (below 1 TB, where the address is the guest's), not the
// import that happens to cover it: imports are 16 MB chunks around what was bound, and since upstream 1c7c9c7 only around
// the bound range, so the pool's end was a chunk's end: the pointer tables of the task and mesh stages (whole pools) and the
// bare entries stopped there, the grass's programs read their records 12-18 MB further on as 0 and culled every blade
static bool game_pool_of(uint64_t gpu, uint64_t& lo, uint64_t& hi) {   // (pools_mu held)
    if (gpu >= (1ull << 40) || guest_pools.empty()) return false;
    auto it = std::upper_bound(guest_pools.begin(), guest_pools.end(), gpu, [](uint64_t a, const GuestPool& p) { return a < p.lo; });
    if (it == guest_pools.begin()) return false;
    --it;
    if (gpu < it->lo || gpu >= it->hi) return false;
    lo = it->lo, hi = it->hi;
    return true;
}
/// True when gpu sits in [lo, hi) and that span was cached from the guest-pool table this generation still is.
bool guest_span_covers(uint64_t gpu, uint64_t lo, uint64_t hi, uint64_t cached_gen, uint64_t gen) {
    return cached_gen == gen && lo < hi && gpu >= lo && gpu < hi;
}
struct GuestSpan { uint64_t lo = 1, hi = 0, gen = 0; };
thread_local GuestSpan guest_span;
bool guest_span_cached(uint64_t gpu, uint64_t* lo, uint64_t* hi) {
    static const bool imported_only = getenv("RX_VK_POOL_END_IMPORTED") != nullptr;   // (A/B: the import's end, as before)
    if (imported_only || gpu >= (1ull << 40)) return false;
    const uint64_t gen = guest_pools_gen.load(std::memory_order_relaxed);
    if (!guest_span_covers(gpu, guest_span.lo, guest_span.hi, guest_span.gen, gen)) return false;
    *lo = guest_span.lo;
    *hi = guest_span.hi;
    return true;
}
void guest_span_store(uint64_t lo, uint64_t hi) {   // (pools_mu held: the generation matches the table just read)
    guest_span = {lo, hi, guest_pools_gen.load(std::memory_order_relaxed)};
}
uint64_t gpu_pool_end(uint64_t gpu) {
    uint64_t lo, hi;
    // (storage-buffer tables ask this per entry, and the entries of one table sit in one pool: the locked lookup of a
    // pool was ~8% of replay time before buffer binds cached it)
    if (guest_span_cached(gpu, &lo, &hi)) return hi;
    ParLock plk;   // (before pools_mu, always)
    std::lock_guard lk(pools_mu);
    static const bool imported_only = getenv("RX_VK_POOL_END_IMPORTED") != nullptr;   // (A/B: the import's end, as before)
    if (!imported_only && game_pool_of(gpu, lo, hi)) {
        guest_span_store(lo, hi);
        return hi;
    }
    const Pool* p = pool_of(gpu, 1);
    return p ? p->hi : 0;
}
uint64_t gpu_pool_start(uint64_t gpu) {   // (EvilOOT) where it begins
    uint64_t lo, hi;
    if (guest_span_cached(gpu, &lo, &hi)) return lo;
    ParLock plk;
    std::lock_guard lk(pools_mu);
    static const bool imported_only = getenv("RX_VK_POOL_END_IMPORTED") != nullptr;
    if (!imported_only && game_pool_of(gpu, lo, hi)) {
        guest_span_store(lo, hi);
        return lo;
    }
    const Pool* p = pool_of(gpu, 1);
    return p ? p->lo : 0;
}

// (pools leaving, a mirror made, or the next submission: a remembered read binding is looked up again)
uint64_t gpu_bind_stamp() {
    return pools_gen.load(std::memory_order_relaxed) ^ (mirror_epoch * 0x9E3779B97F4A7C15ull);
}

uint32_t bind_slot(uint64_t guest, uint64_t size) {
    // (a page-aligned range's low bits are zero: fold the rest in, or every one of them lands in one slot)
    uint64_t h = guest ^ (size * 0x9E3779B97F4A7C15ull);
    h *= 0x9E3779B97F4A7C15ull;
    h ^= h >> 32;
    h ^= h >> 16;
    return (uint32_t)(h & 255u);
}

bool bind_memo_hit(uint64_t guest, uint64_t size, uint64_t stamp, uint64_t cached_guest, uint64_t cached_size,
                   uint64_t cached_stamp, int has_buffer) {
    return has_buffer && size && guest == cached_guest && size == cached_size && stamp == cached_stamp;
}

struct BindMemo {
    uint64_t guest = 0, size = 0, stamp = 0;
    BufferRef ref{};
};
thread_local BindMemo bind_memo[256];

/// (Triforce) A binding that reaches a page whose sync was put off (deferred_late): its write-back is waited for and the page
/// goes from guest memory into its pool's mirror now, in this part's uploads: what take_capture did for every such page
void late_deferred_bound(uint64_t guest, uint64_t size) {
    const uint64_t p0 = guest >> kPageShift, p1 = (guest + (size ? size - 1 : 0)) >> kPageShift;
    static thread_local std::vector<uint32_t> hit;
    hit.clear();
    {
        std::lock_guard dk(late_defer_mu);
        std::erase_if(deferred_late, [&](uint32_t page) {
            if (page < p0 || page > p1) return false;
            hit.push_back(page);
            return true;
        });
        deferred_late_n.store((int)deferred_late.size(), std::memory_order_relaxed);
    }
    for (uint32_t page : hit) {
        uint64_t v = 0;
        {
            std::lock_guard wk(wb_pages_mu);
            v = wb_pages.get(page);
        }
        const uint64_t done0 = vk_timeline_completed();
        const auto w0 = std::chrono::steady_clock::now();
        if (v > done0) vk_timeline_wait(v);
        st_late_jit++;
        {   // (diagnostics, RX_VK_LOG_LATE: the bindings that reached a page put off, and what they waited)
            static const bool log_late = getenv("RX_VK_LOG_LATE") != nullptr;
            static int logged;
            static int64_t seen;
            if (log_late && ++seen > 600 && logged++ < 24)
                logf("vk: late: page %#llx put off, bound by %#llx +%#llx: waited %.2f ms (value %llu, done %llu)",
                     (unsigned long long)page << kPageShift, (unsigned long long)guest, (unsigned long long)size,
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - w0).count(),
                     (unsigned long long)v, (unsigned long long)done0);
        }
        std::lock_guard pk(pools_mu);
        const uint64_t at = (uint64_t)page << kPageShift;
        auto it = imported.upper_bound(at);
        if (it == imported.begin()) continue;
        --it;
        Pool& p = it->second;
        if (at >= p.hi || p.mstate != Pool::MIRROR_ON || !p.mirror || !mirror_upload) continue;   // (no mirror: the GPU reads guest memory)
        const uint64_t a = std::max(at, p.lo), b = std::min(at + 4096, p.hi);
        if (!pieces_resident(&p, a - p.lo, b - a)) continue;   // (Triforce: no memory under it: the GPU reads guest memory there)
        const VkBufferCopy r{a - p.lo, a - p.lo, b - a};
        vkCmdCopyBuffer(upload_command_buffer(), p.buffer, p.mirror, 1, &r);
        st_sync_bytes += (int64_t)r.size, st_sync_copies++;
    }
}

// (Triforce, RX_VK_LOG_MIRROR_USE=1, a diagnostic: how much of each mirrored pool the bindings reach. A mirror is its whole
// import, 16 MB chunks of the game's address space, whatever part of a chunk the game binds; after a tour of ten areas the
// Market's mirrors were at their 3 GB budget against 1.5 GB fresh. A bit per 64 KB bound since the last report (the bindings'
// memo is off meanwhile: every binding is seen); what shaders reach through the page table is not counted)
bool mirror_use_on() {
    static const bool v = getenv("RX_VK_LOG_MIRROR_USE") != nullptr;
    return v;
}
std::mutex mirror_use_mu;
std::unordered_map<uint64_t, std::vector<uint64_t>> mirror_use;   // (a pool's lo -> its bits)
void mirror_use_note(const Pool* p, uint64_t off, uint64_t size) {
    if (!size) return;
    std::lock_guard lk(mirror_use_mu);
    std::vector<uint64_t>& bits = mirror_use[p->lo];
    const uint64_t n = (p->hi - p->lo + 65535) >> 16;
    if (bits.size() < (n + 63) / 64) bits.resize((n + 63) / 64);
    for (uint64_t q = off >> 16; q <= std::min((off + size - 1) >> 16, n - 1); q++) bits[q >> 6] |= 1ull << (q & 63);
}
void mirror_use_report() {   // (pools_mu held)
    if (!mirror_use_on()) return;
    std::lock_guard lk(mirror_use_mu);
    uint64_t on_bytes = 0, bound_bytes = 0, unbound_bytes = 0, tenth_bytes = 0;
    size_t on = 0, unbound = 0, tenth = 0;
    std::vector<std::pair<uint64_t, std::string>> rows;   // (the mirrors by what they hold unbound)
    for (auto& [lo, p] : imported) {
        if (p.mstate != Pool::MIRROR_ON) continue;
        uint64_t used = 0;
        if (const auto it = mirror_use.find(lo); it != mirror_use.end())
            for (const uint64_t w : it->second) used += (uint64_t)__builtin_popcountll(w) << 16;
        const uint64_t n = p.hi - p.lo;
        used = std::min(used, n);
        on++, on_bytes += n, bound_bytes += used;
        if (!used) unbound++, unbound_bytes += n;
        else if (used * 10 < n) tenth++, tenth_bytes += n;
        char b[160];
        snprintf(b, sizeof b, " %#llx %llu MB: %llu KB bound, idle %llu;", (unsigned long long)lo, (unsigned long long)(n >> 20),
                 (unsigned long long)(used >> 10), (unsigned long long)(mirror_epoch - p.synced_epoch));
        rows.push_back({n - used, b});
    }
    mirror_use.clear();
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::string top;
    for (size_t i = 0; i < rows.size() && i < 24; i++) top += rows[i].second;
    logf("vk: mirror use (1200 submissions): %zu mirrors, %llu MB; bound in 64 KB pieces %llu MB; never bound %zu (%llu MB); under a "
         "tenth bound %zu (%llu MB); the most unbound:%s", on, (unsigned long long)(on_bytes >> 20), (unsigned long long)(bound_bytes >> 20),
         unbound, (unsigned long long)(unbound_bytes >> 20), tenth, (unsigned long long)(tenth_bytes >> 20), top.c_str());
}

BufferRef gpu_buffer_bind(uint64_t gpu, uint64_t size, bool written) {
    if (written) texture_gpu_wrote(gpu, size);
    // (below 1 TB a GPU address is the guest address itself, and a pool holds the whole range or nothing: the host's
    // two translations are for the virtual pools above)
    uint64_t guest = gpu;
    if (gpu >= (1ull << 40)) {
        uint64_t avail = 0;
        if (!host->gpu_ptr(gpu, &avail) || avail < size) return {};
        guest = host->gpu_guest(gpu);
    }
    if (deferred_late_n.load(std::memory_order_relaxed)) late_deferred_bound(guest, size);   // (Triforce: a page put off, bound now)
    // (the same read, while that stamp still stands: the buffer and the offset are the ones found before. a miss
    // runs the lookup below, including a mirror sync, and remembers what it returned. a write is not remembered)
    auto remember = [&](BufferRef ref) {
        if (!written && ref.buffer != VK_NULL_HANDLE && size)
            bind_memo[bind_slot(guest, size)] = BindMemo{guest, size, gpu_bind_stamp(), ref};
        return ref;
    };
    if (!written && !mirror_use_on()) {
        const BindMemo& m = bind_memo[bind_slot(guest, size)];
        if (bind_memo_hit(guest, size, gpu_bind_stamp(), m.guest, m.size, m.stamp, m.ref.buffer != VK_NULL_HANDLE))
            return m.ref;
    }
    // (a read binding of a pool this thread found before and whose mirror is in use this submission or that has none,
    // without the lock: every thread's, the replay thread's too (each draw binds several uniform buffers: the pool's
    // lookup under the lock was ~8% of the replay's time in a race); the rest under it, as below)
    const bool par = par_active.load(std::memory_order_relaxed);
    TlPool& tl = tl_pools[(guest >> 16) & 1023];
    if (!written && tl.p && tl.gen == pools_gen.load(std::memory_order_relaxed) && guest >= tl.p->lo &&
        guest <= tl.p->hi && size <= tl.p->hi - guest) {   // (hookshot: no wrap-around for a huge size, as pool_of)
        Pool* p = tl.p;
        const uint64_t off = guest - p->lo;
        if (p->last_value < mem_rec_value) p->last_value = mem_rec_value;
        __atomic_fetch_add(&p->binds, 1u, __ATOMIC_RELAXED);
        if (mirror_use_on()) mirror_use_note(p, off, size);
        if (!mirror_on() || !mirror_upload || p->mstate == Pool::MIRROR_REFUSED || p->mstate == Pool::MIRROR_WANTED)
            return remember({p->buffer, off, size, 0});
        if (p->mstate == Pool::MIRROR_ON && p->synced_epoch == mirror_epoch && p->sync_seg <= texture_segment()) {
            // (Triforce: a mirror in pieces: only where it has memory; a piece left in guest memory for good: the import; else below)
            if (pieces_resident(p, off, size)) return remember({p->mirror, off, size, 0});
            if (pieces_direct(p, off, size)) return remember({p->buffer, off, size, 0});
        }
    }
    ParLock plk;   // (before pools_mu, always: a mirror sync records into the shared upload command buffer)
    std::lock_guard lk(pools_mu);
    {   // (hookshot, smoothness: import_in_background above. Reads up to 4 MB; not in a parallel replay's segment)
        static const bool async_import = [] { const char* e = getenv("RX_VK_ASYNC_IMPORT"); return e && atoi(e) != 0; }();
        if (async_import && !written && !par && size <= (4u << 20) && guest < (1ull << 40) &&
            !(last_pool && guest >= last_pool->lo && guest + size <= last_pool->hi) && import_in_background(guest, size)) {
            uint64_t avail = 0;
            const uint8_t* src = host->gpu_ptr(gpu, &avail);
            if (src && avail >= size)
                if (const BufferRef r = stream_copy(src, size)) {
                    st_async_copies++;
                    return r;
                }
        }
    }
    Pool* p = pool_of(guest, size);
    if (!p) return {};
    tl = {p, pools_gen.load(std::memory_order_relaxed)};
    if (p->last_value < mem_rec_value) p->last_value = mem_rec_value;
    __atomic_fetch_add(&p->binds, 1u, __ATOMIC_RELAXED);
    const uint64_t off = guest - p->lo;
    if (mirror_use_on()) mirror_use_note(p, off, size);
    if (!mirror_on() || !mirror_upload || p->mstate == Pool::MIRROR_REFUSED) {
        if (mirror_on()) { auto& e = direct_by_pool[p->lo]; e.first++, e.second = mirror_upload ? (int)p->mstate : 9; direct_bytes_by_pool[p->lo] += size; }
        // (hookshot) written by the GPU in guest memory itself, past the mirror (bound outside a recording's uploads): the
        // write-back diff's shadow no longer knows those pages, and reads them again at its next write-back, after this write
        if (written) shadow_forget(*p, off, std::min(size, p->hi - p->lo - off));
        return remember({p->buffer, off, size, 0});
    }
    // (a pool's mirror is made at its first binding, in this part's uploads: bound in guest memory until the next
    // submission, the GPU read it there while the game's threads wrote it for their next frames, and a pass that drew
    // over the moment a write landed came out in two (shadows dark below a line for a frame, the line moving; a new
    // pool every so many frames as the per-frame data moves on). Not when this part already wrote it in guest memory:
    // the copy at the part's start would miss that. Not in a parallel replay's segment either: its copy goes into the
    // uploads of the segment whose worker bound the pool first, which need not be the first in the queue. A segment
    // before it found the mirror in the page table and read it unfilled (houses, rocks and bushes missing, geometry
    // torn, the more the segments the worse; #994 with RX_VK_PARALLEL). RX_VK_MIRROR_DEFER=1: at the next submission, as before)
    static const bool defer = getenv("RX_VK_MIRROR_DEFER") != nullptr;
    if (p->mstate == Pool::MIRROR_NONE && !defer && !par && p->direct_write_epoch != mirror_epoch && mirror_make(*p)) {
        publish_cap_ranges();
        static int64_t made_now;
        if (++made_now % 200 == 1)
            logf("vk: pool %#llx (%llu MB): mirror made at its first binding (%lld so)", (unsigned long long)p->lo,
                 (unsigned long long)((p->hi - p->lo) >> 20), (long long)made_now);
    }
    if (p->mstate == Pool::MIRROR_NONE) {   // (made at the next submission's start)
        p->mstate = Pool::MIRROR_WANTED;
        mirror_wanted.push_back(p);
    }
    if (p->mstate != Pool::MIRROR_ON) {
        auto& e = direct_by_pool[p->lo];
        e.first++, e.second = (int)p->mstate;
        direct_bytes_by_pool[p->lo] += size;
        if (written) p->direct_write_epoch = mirror_epoch;
        if (written && size <= (1u << 20) && !tl_event_write)   // (EvilBoyce: GPU results written in place; small ones: the readbacks are)
            wb_pages_note(p->lo + off, p->lo + std::min(off + size, p->hi - p->lo), mem_rec_value);
        return remember({p->buffer, off, size, 0});
    }
    if (p->synced_epoch != mirror_epoch || p->sync_seg > texture_segment()) mirror_sync(*p);
    // (Triforce) a mirror in pieces: the pieces this binding reaches get their memory now, filled in this recording's uploads.
    // Not in a parallel replay's segment (its uploads are not ahead of the segments before it: the reason a mirror is not made
    // there either), nor past the budget: the import this time, as an unmirrored pool's binding
    // A piece left in guest memory for good (the CPU and the GPU both write it: demote_trials) makes its bindings read the
    // import, as a pool out of the mirrors does
    if (p->mpieces && !pieces_resident(p, off, size)) {
        const bool direct = pieces_direct(p, off, size);
        if (direct || par || !mirror_pieces_make(*p, off, size, true)) {
            if (!direct) st_piece_direct++;
            if (!direct && par && piece_wanted.size() < 4096) piece_wanted.push_back({p->lo, off, size});
            auto& e = direct_by_pool[p->lo];
            e.first++, e.second = direct ? 5 : 4;
            direct_bytes_by_pool[p->lo] += size;
            if (written) {
                shadow_forget(*p, off, std::min(size, p->hi - p->lo - off));
                if (!direct && size <= (1u << 20) && !tl_event_write)
                    wb_pages_note(p->lo + off, p->lo + std::min(off + size, p->hi - p->lo), mem_rec_value);
            }
            return direct ? remember({p->buffer, off, size, 0}) : BufferRef{p->buffer, off, size, 0};
        }
    }
    if (written) {
        if (tl_from_compute) p->compute_writes = true;
        // (hookshot, 11-10) RX_VK_WB_MAX_KB=<KB>: bindings larger than that do not queue a write-back. OoT writes
        // tens of MB of culling lists and per-frame buffers through the GPU that the CPU never reads; copying them
        // back was most of a submit's memory sync (issue 384: 68 MB a frame, 7 ms of the GPU's time, and
        // RX_VK_NO_WRITEBACK took an RTX 4080 from 45 to 57 fps). Small results the CPU does read (the exposure
        // meter and the like) stay copied. 0 or unset: every written binding, as before.
        static const uint64_t wb_max = [] {
            const char* e = getenv("RX_VK_WB_MAX_KB");
            const uint64_t kb = e ? (uint64_t)std::max(0, atoi(e)) : 0;
            if (kb) logf("vk: write-backs only of written bindings up to %llu KB (RX_VK_WB_MAX_KB)", (unsigned long long)kb);
            return kb << 10;
        }();
        if (!wb_max || size <= wb_max)
            writebacks.push_back({p, off, std::min(off + size, p->hi - p->lo), tl_event_write});
        rb_hazard_locked(p->lo + off, p->lo + std::min(off + size, p->hi - p->lo));   // (memory_rb_refresh)
        const uint64_t np = (p->hi - p->lo + 4095) >> 12;
        if (p->gpu_bits.size() < (np + 63) / 64) p->gpu_bits.resize((np + 63) / 64);
        const uint64_t a = off >> 12, b = std::min((off + std::max<uint64_t>(size, 1) - 1) >> 12, np - 1);
        for (uint64_t q = a; q <= b; q++) p->gpu_bits[q >> 6] |= 1ull << (q & 63);
    }
    return remember({p->mirror, off, size, 0});
}

// ---- mirrors given back ----
// A mirror was never freed: the retired pools' stayed (every pool remade larger, or taken in by a larger one, kept
// its VRAM), and pools the game no longer used kept theirs until the mirrors' budget was full; the pools after that
// stayed in system memory for good (the GPU read them over PCIe). Now a retired pool's mirror goes once the GPU is done
// with the submissions that bound it; a mirror not bound for kIdleEpochs submissions goes when a wanted one does not
// fit, or after kLongIdle; and a pool refused for the budget is tried again when it is next bound.
constexpr uint64_t kIdleEpochs = 600, kLongIdle = 6000;
int64_t st_mirrors_freed = 0;
uint64_t st_mirror_bytes_freed = 0;
// (hookshot) A freed mirror's buffer is destroyed only once the GPU is past everything recorded until then. Its pool's
// last_value counts the draws and dispatches that bound it, not the shaders that reach it through the guest page table:
// a culling or fog compute pass already running read a mirror destroyed under it (device lost in 53caac5f and ee7764a6,
// issues 132 and 195, AMD after a scene change). The table goes back to guest memory at once, as before
struct DeadMirror { VkBuffer buffer; VmaAllocation alloc; uint64_t value; std::vector<VmaAllocation> pieces; };   // (pieces: a sparse one's memory)
std::deque<DeadMirror> dead_mirrors;
// A retired pool's import, given back the same way. Each import is a userptr buffer object to amdgpu, and RADV lists every
// buffer object in every submission (buffer device addresses): the kernel checks every page of every userptr object at each
// DRM_IOCTL_AMDGPU_CS. Retired pools were never given back, so the pages it checked only grew with each pool merge: in Kokiri
// Forest ~1.3 GB of imports, 15-45 ms a submission (3-4 fps on an RX 7800 XT with 11 ms of GPU time; issues 312, 413). Its
// table entries went at its retirement, and its mirror goes before (mstate), so once the GPU is past retired_value nothing
// reaches it. The node stays (pointers to it are kept elsewhere); only its buffer and memory go. RX_VK_KEEP_RETIRED=1: kept
struct DeadImport { VkBuffer buffer; VkDeviceMemory memory; uint64_t value; uint64_t bytes; std::vector<VkDeviceMemory> pieces; };
std::deque<DeadImport> dead_imports;
uint64_t st_retired_imports_freed = 0, st_retired_import_bytes_freed = 0;
void reap_mirrors() {   // (pools_mu held)
    while (!dead_mirrors.empty() && dead_mirrors.front().value <= mem_completed) {
        if (getenv("RX_VK_LOG_ADDR")) {
            VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
            da.buffer = dead_mirrors.front().buffer;
            logf("vk: addr mirror freed at gpu %#llx (value %llu, completed %llu)", (unsigned long long)vkGetBufferDeviceAddress(dev.device, &da),
                 (unsigned long long)dead_mirrors.front().value, (unsigned long long)mem_completed);
        }
        if (dead_mirrors.front().alloc) {
            destroy_buffer(dead_mirrors.front().buffer, dead_mirrors.front().alloc);
        } else {   // (Triforce: a mirror in pieces: not while a bind of its is still to be made; the buffer, then its memory)
            {
                std::lock_guard bk(piece_binds_mu);
                bool pending = false;
                for (const PieceBind& b : piece_binds) pending = pending || b.buffer == dead_mirrors.front().buffer;
                if (pending) break;
            }
            vkDestroyBuffer(dev.device, dead_mirrors.front().buffer, nullptr);
            for (VmaAllocation a : dead_mirrors.front().pieces) vma_piece_free(a);
        }
        dead_mirrors.pop_front();
    }
    while (!dead_imports.empty() && dead_imports.front().value <= mem_completed) {
        vkDestroyBuffer(dev.device, dead_imports.front().buffer, nullptr);
        vkFreeMemory(dev.device, dead_imports.front().memory, nullptr);
        for (VkDeviceMemory m : dead_imports.front().pieces) vkFreeMemory(dev.device, m, nullptr);
        st_retired_imports_freed++, st_retired_import_bytes_freed += dead_imports.front().bytes;
        dead_imports.pop_front();
    }
}
void retired_imports_give_back() {   // (pools_mu held)
    // (hookshot) Kept by default on Windows: giving them back is for the Linux amdgpu submit above, and on Windows an Intel Arc
    // B390 lost the device entering Kokiri every time with it and 1 run in 5 without (issue 17, fuxau); the AMD device losses
    // on the Ally X (309) and RX 6600 (571) came right after retired pools went back. RX_VK_KEEP_RETIRED=0 or 1 chooses
#ifdef _WIN32
    // (not on NVIDIA: kept there, ~20 areas of play left 6.8 GB of retired pools imported on an RTX 3070 (8 GB), and the next area
    // lost the device, twice; given back, the same tour finished every area (issue 1057). The losses above were Intel's and AMD's)
    static const bool keep = getenv("RX_VK_KEEP_RETIRED") ? getenv("RX_VK_KEEP_RETIRED")[0] != '0' : dev.props.vendorID != 0x10DE;
#else
    static const bool keep = getenv("RX_VK_KEEP_RETIRED") && getenv("RX_VK_KEEP_RETIRED")[0] != '0';
#endif
    uint64_t live_bytes = 0, retired_bytes = 0;
    size_t retired_alive = 0;
    for (auto& [lo, p] : imported) live_bytes += p.hi - p.lo;
    for (auto& nh : retired_pools) {
        Pool& p = nh.mapped();
        if (!p.memory) continue;
        if (!keep && p.mstate == Pool::MIRROR_NONE && !p.inherit_pending && p.retired_value <= mem_completed &&
            p.last_value <= mem_completed) {
            dead_imports.push_back({p.buffer, p.memory, std::max(mem_rec_value, p.last_value), p.hi - p.lo, std::move(p.pieces)});
            p.pieces.clear();
            p.buffer = VK_NULL_HANDLE, p.memory = VK_NULL_HANDLE, p.address = 0;
            pools_gen++;
            continue;
        }
        retired_alive++, retired_bytes += p.hi - p.lo;
    }
    static uint64_t logged_epoch;
    if (mirror_epoch - logged_epoch >= 1200) {
        logged_epoch = mirror_epoch;
        mirror_use_report();
        logf("vk: imports: %zu live (%llu MB), %zu retired still imported (%llu MB), %llu retired given back (%llu MB)",
             imported.size(), (unsigned long long)(live_bytes >> 20), retired_alive, (unsigned long long)(retired_bytes >> 20),
             (unsigned long long)st_retired_imports_freed, (unsigned long long)(st_retired_import_bytes_freed >> 20));
    }
}
void mirror_free(Pool& p) {
    p.was_mirrored = true;
    {   // (the table back to guest memory, for a pool still in use only: a retired one's range is another pool's now, and its
        // address there sent the foliage culling into freed memory, a device lost)
        const auto live = imported.find(p.lo);
        if (live != imported.end() && &live->second == &p) guest_pages_set(p.lo, p.hi, p.address);
    }
    const uint64_t held = p.mpieces ? p.res_bytes : p.hi - p.lo;   // (Triforce: in pieces, what its pieces hold)
    dead_mirrors.push_back({p.mirror, p.mirror_alloc, std::max(mem_rec_value, p.last_value), std::move(p.res_allocs)});
    shadow_free(p);
    p.mirror = VK_NULL_HANDLE, p.mirror_alloc = nullptr;
    if (p.mpieces) {   // (the bits cleared in place: gpu_buffer_bind reads them without the lock)
        for (uint64_t& w : p.res_bits) __atomic_store_n(&w, 0ull, __ATOMIC_RELAXED);
        p.res_allocs.clear(), p.res_bytes = 0, p.mpieces = false, p.mirror_address = 0;
    }
    mirror_bytes -= std::min(mirror_bytes, held);
    st_mirror_bytes_freed += held;
    st_mirrors_freed++;
    p.mstate = Pool::MIRROR_NONE;
    pools_gen++;   // (a parallel replay thread's pool cache: looked up again)
}
void mirrors_give_back(uint64_t want_bytes) {   // (pools_mu held)
    static const bool keep = getenv("RX_VK_MIRROR_KEEP") != nullptr;   // (A/B: mirrors kept, as before)
    if (keep) return;
    for (auto& nh : retired_pools) {
        Pool& p = nh.mapped();
        // (EvilOOT: kept while the pool that took it in has not made its mirror, for 600 submissions at most)
        if (p.inherit_pending && mirror_epoch - p.retired_epoch > kIdleEpochs) p.inherit_pending = false;
        if (p.mstate == Pool::MIRROR_ON && p.retired_value <= mem_completed && !p.inherit_pending) mirror_free(p);
    }
    std::vector<Pool*> idle;
    for (auto& [lo, p] : imported)
        if (p.mstate == Pool::MIRROR_ON && mirror_epoch - p.synced_epoch > kIdleEpochs && p.last_value <= mem_completed)
            idle.push_back(&p);
    std::sort(idle.begin(), idle.end(), [](const Pool* a, const Pool* b) { return a->synced_epoch < b->synced_epoch; });
    for (Pool* p : idle)
        if (mirror_epoch - p->synced_epoch > kLongIdle || mirror_bytes + want_bytes > mirror_budget()) mirror_free(*p);
    static int64_t logged_at;
    if (st_mirrors_freed / 16 != logged_at) {
        logged_at = st_mirrors_freed / 16;
        logf("vk: %lld mirrors given back (%llu MB in all); %llu MB of mirrors now", (long long)st_mirrors_freed,
             (unsigned long long)(st_mirror_bytes_freed >> 20), (unsigned long long)(mirror_bytes >> 20));
    }
}

/// (Evil Hookshot) OoT's GPU particles (falling leaves, fireflies, floating debris): the game reads back on the CPU what a
/// compute pass wrote, in pools it also writes; with a VRAM mirror the CPU read stale data and every particle was dead.
/// A pool whose CPU and GPU pages collide (mirror_sync) is tried in guest memory, one at a time: 40 frames of GPU time
/// before, 10 to settle, 90 after; more than 20% (RX_VK_DEMOTE_COST=<percent>) + 0.5 ms slower, its mirror comes back (a
/// second time: for this run; 1.5 times slower: in triforce_mirror_keep.txt, for later runs too)
/// (pools_mu held, at a submission's start)
/// (Evil Hookshot) pools whose trials were too costly, kept across runs in triforce_mirror_keep.txt beside the executable
/// (hex starts, one a line): the game's pools sit at the same addresses every run, and each trial is a short stutter.
/// Delete the file to try them all again
std::string mirror_keep_path() {
    char exe[MAX_PATH];
    const DWORD n = game_module_path(exe, MAX_PATH);   // (vk_paths.h: the game folder, wherever the executable is)
    std::string dir(exe, n);
    return dir.substr(0, dir.find_last_of("\\/") + 1) + "triforce_mirror_keep.txt";
}
std::vector<uint64_t>& mirror_keep() {
    static std::vector<uint64_t> v = [] {
        std::vector<uint64_t> r;
        if (FILE* f = fopen(mirror_keep_path().c_str(), "r")) {
            char line[64];
            while (fgets(line, sizeof line, f))
                if (const uint64_t a = strtoull(line, nullptr, 16)) r.push_back(a);
            fclose(f);
        }
        if (!r.empty()) logf("vk: %zu pools kept mirrored from earlier trials (%s)", r.size(), mirror_keep_path().c_str());
        return r;
    }();
    return v;
}
void mirror_keep_add(uint64_t lo) {
    if (std::find(mirror_keep().begin(), mirror_keep().end(), lo) != mirror_keep().end()) return;
    mirror_keep().push_back(lo);
    if (FILE* f = fopen(mirror_keep_path().c_str(), "a")) fprintf(f, "%llx\n", (unsigned long long)lo), fclose(f);
}
/// (Triforce, RX_VK_DEMOTE_PIECES=2) A pool whose trial says it stays out of the mirrors, and that had a mirror in pieces:
/// mirrored again (at its next binding) without the pieces holding pages both write; those found later leave at once
/// (mirror_sync). Returns the KB that stay in guest memory; 0: not such a pool, the caller leaves it out whole. (pools_mu held)
uint64_t stays_in_pieces(Pool& p) {
    if (demote_pieces_mode() != 2 || p.direct_bits.empty() || p.conf_bits.size() != p.direct_bits.size()) return 0;
    uint64_t n = 0;
    for (const uint64_t w : p.conf_bits) n += (uint64_t)std::popcount(w);
    if (!n) return 0;
    if (p.mstate == Pool::MIRROR_ON) pieces_set_direct(p, p.conf_bits, true);   // (phase 4: mirrored again meanwhile)
    else
        for (size_t w = 0; w < p.conf_bits.size(); w++) __atomic_fetch_or(&p.direct_bits[w], p.conf_bits[w], __ATOMIC_RELAXED);
    std::fill(p.conf_bits.begin(), p.conf_bits.end(), 0ull);
    p.direct_auto = true;
    if (p.mstate == Pool::MIRROR_REFUSED) p.mstate = Pool::MIRROR_NONE;
    p.demote_queued = true;   // (no further trial: such pieces leave as they are found)
    pools_gen++;
    return (n << p.piece_shift) >> 10;
}
void demote_trials() {
    // (phases: 1 settling in guest memory, 2 measured there, 3 settling mirrored again, 4 measured mirrored again)
    struct Trial { uint64_t lo = 0; int phase = 0; uint64_t f = 0; int64_t g = 0; double base_ms = 0, after_ms = 0; };
    static Trial t;
    static uint64_t base_f = 0;    // (the GPU time's last checkpoint while no trial runs)
    static int64_t base_g = 0;
    static double base_ms = -1;
    static const double cost = getenv("RX_VK_DEMOTE_COST") ? atof(getenv("RX_VK_DEMOTE_COST")) / 100.0 : 0.20;
    const uint64_t f = texture_frame_no();
    const int64_t g = gpu_time_total_ns();
    const auto pool_at = [](uint64_t lo) -> Pool* {
        auto it = imported.find(lo);
        return it == imported.end() ? nullptr : &it->second;
    };
    if (t.phase == 0) {
        if (f >= base_f + 40) {   // (a checkpoint every 40 frames: the baseline is the last window's)
            if (base_f && f - base_f < 400) base_ms = (double)(g - base_g) / (double)(f - base_f) / 1e6;
            base_f = f, base_g = g;
        }
        // (a baseline from the game's own frames: a load or a menu takes well under 3 ms, and a pool then looked costly)
        while (!demote_wanted.empty() && base_ms >= 3.0) {
            const uint64_t lo = demote_wanted.front();
            demote_wanted.erase(demote_wanted.begin());
            Pool* p = pool_at(lo);
            if (!p || p->mstate != Pool::MIRROR_ON || p->demote_blocked) continue;
            if (std::find(mirror_keep().begin(), mirror_keep().end(), lo) != mirror_keep().end()) { p->demote_blocked = true; continue; }
            {   // (Triforce) the size cap once more, on the pool as it is now: the list holds starts, and a 48 MB pool queued
                // before it was remade as a 224 MB one took that one out of the mirrors whole (see below)
                static const uint64_t cap_mb = getenv("RX_VK_DEMOTE_MB") ? strtoull(getenv("RX_VK_DEMOTE_MB"), nullptr, 10) : 64;
                if (!(p->mpieces && demote_pieces_mode() == 1) && p->hi - p->lo > (cap_mb << 20)) {
                    p->demote_queued = false, p->conflict_syncs = 0;
                    std::fill(p->conf_bits.begin(), p->conf_bits.end(), 0ull);
                    continue;
                }
            }
            if (p->mpieces && demote_pieces_mode() == 1) {
                // (Triforce) A mirror in pieces: only the pieces holding pages both write leave it. The whole pool went before:
                // a 224 MB one tried in an area that hardly used it "stayed in guest memory" for the session, and at the Market
                // the GPU then read 2.5 MB of it a submission over PCIe: its frame 13.7 ms against 9.2 (r_S2b, 2026-11-11)
                bool any = false;
                for (const uint64_t w : p->conf_bits) any = any || w;
                if (!any) {
                    p->demote_queued = false, p->conflict_syncs = 0;
                    continue;
                }
                p->trial_bits = p->conf_bits;
                std::fill(p->conf_bits.begin(), p->conf_bits.end(), 0ull);
                pieces_set_direct(*p, p->trial_bits, true);
                pools_gen++;
            } else {
                mirror_free(*p);
                p->mstate = Pool::MIRROR_REFUSED;
            }
            t = {lo, 1, f, g, base_ms};
            break;
        }
        return;
    }
    if ((t.phase == 1 || t.phase == 3) && f >= t.f + 10) { t.phase++, t.f = f, t.g = g; return; }
    const double so_far = (t.phase == 2 || t.phase == 4) && f > t.f ? (double)(g - t.g) / (double)(f - t.f) / 1e6 : 0;
    // (and an absolute budget: each pool out of the mirrors that stays costs a little, ~0.3-1 ms for the per-frame transient
    // pools, too little to see alone: 13 -> 18 ms after six of them. One may add 0.5 ms, all of them 0.75)
    static double kept_ms = 0;
    const auto too_slow = [&](double after, double base) {
        return after > base * (1.0 + cost) + 0.5 || after - base > 0.5 || kept_ms + std::max(0.0, after - base) > 0.75;
    };
    const auto finish = [&] {
        t = {};
        base_f = f, base_g = g;   // (a fresh baseline for the next one)
        base_ms = -1;
    };
    if (t.phase == 2 && (f >= t.f + 90 || (f >= t.f + 10 && so_far > t.base_ms * 2.0 + 1.0))) {   // (twice as slow: back at once)
        const double after = so_far;
        Pool* p = pool_at(t.lo);
        if (!p) return finish();
        const auto trial_kb = [&] {   // (in pieces: what the trial's pieces span)
            uint64_t n = 0;
            for (const uint64_t w : p->trial_bits) n += (uint64_t)std::popcount(w);
            return (n << p->piece_shift) >> 10;
        };
        if (!too_slow(after, t.base_ms)) {
            if (const uint64_t kb = stays_in_pieces(*p)) {   // (RX_VK_DEMOTE_PIECES=2)
                kept_ms += std::max(0.0, after - t.base_ms);
                p->gpu_bits.clear();
                logf("vk: pool %#llx (%llu MB): the CPU and the GPU both write it; in guest memory the GPU took %.2f ms a frame, %.2f "
                     "before: %llu KB of it stay in guest memory, the rest is mirrored again", (unsigned long long)t.lo,
                     (unsigned long long)((p->hi - p->lo) >> 20), after, t.base_ms, (unsigned long long)kb);
                return finish();
            }
        }
        if (!too_slow(after, t.base_ms)) {
            kept_ms += std::max(0.0, after - t.base_ms);
            p->gpu_bits.clear();
            if (!p->trial_bits.empty()) {   // (its other pieces stay mirrored, and may be tried when both write them too)
                logf("vk: pool %#llx (%llu MB): the CPU and the GPU both write %llu KB of it; with those pieces in guest memory the GPU "
                     "took %.2f ms a frame, %.2f before: they stay in guest memory", (unsigned long long)t.lo,
                     (unsigned long long)((p->hi - p->lo) >> 20), (unsigned long long)trial_kb(), after, t.base_ms);
                p->trial_bits.clear();
                p->demote_queued = false, p->conflict_syncs = 0;
                return finish();
            }
            logf("vk: pool %#llx (%llu MB): the CPU and the GPU both write it; in guest memory the GPU took %.2f ms a frame, %.2f "
                 "before: stays in guest memory", (unsigned long long)t.lo, (unsigned long long)((p->hi - p->lo) >> 20), after, t.base_ms);
            return finish();
        }
        if (!p->trial_bits.empty()) {   // (Triforce: the pieces back among those the mirror may hold: made again at their next binding)
            pieces_set_direct(*p, p->trial_bits, false);
            pools_gen++;
            t.after_ms = after, t.phase = 3, t.f = f, t.g = g;
            return;
        }
        // (triforce) Slower, by the window before. That window can be off: 7.75 ms just after a load against 13.85 in guest memory,
        // and the falling leaves' pool went back to its mirror, the leaves dead again. So the pool is mirrored again (at its next
        // binding: mirror_make copies what the GPU wrote in guest memory) and the GPU's time measured once more that way; only
        // when that is clearly faster is the cost real
        p->mstate = Pool::MIRROR_NONE;
        pools_gen++;
        t.after_ms = after, t.phase = 3, t.f = f, t.g = g;
        return;
    }
    if (t.phase == 4 && f >= t.f + 40) {
        const double back = so_far, after = t.after_ms;
        Pool* p = pool_at(t.lo);
        if (!p) return finish();
        const bool slow = too_slow(after, back);   // (against the mirror as it is now)
        if (slow) {
            if (++p->demote_fails >= 2) {
                p->demote_blocked = true;
                if (after > back * 1.5) mirror_keep_add(t.lo);   // (only the clearly costly ones for later runs)
            }
            else p->demote_queued = false, p->conflict_syncs = 0;   // (one more trial later: a scene change can fool one)
        } else if (!p->trial_bits.empty()) {   // (Triforce: those pieces out of the mirror again, for good)
            pieces_set_direct(*p, p->trial_bits, true);
            pools_gen++;
            kept_ms += std::max(0.0, after - back);
            p->demote_queued = false, p->conflict_syncs = 0;
        } else if (stays_in_pieces(*p)) {   // (RX_VK_DEMOTE_PIECES=2: mirrored, without its pieces both write)
            kept_ms += std::max(0.0, after - back);
        } else {   // (out of the mirror again, for good)
            if (p->mstate == Pool::MIRROR_ON) mirror_free(*p);
            p->mstate = Pool::MIRROR_REFUSED;
            pools_gen++;
            kept_ms += std::max(0.0, after - back);
        }
        const bool in_pieces = !p->trial_bits.empty();
        std::fill(p->conf_bits.begin(), p->conf_bits.end(), 0ull);   // (found again from the pages the GPU writes from here on)
        uint64_t kb = 0;
        for (const uint64_t w : p->trial_bits) kb += ((uint64_t)std::popcount(w) << p->piece_shift) >> 10;
        p->trial_bits.clear();
        p->gpu_bits.clear();
        if (in_pieces)
            logf("vk: pool %#llx (%llu MB): the CPU and the GPU both write %llu KB of it; with those pieces in guest memory the GPU took "
                 "%.2f ms a frame, %.2f before, %.2f mirrored again: %s", (unsigned long long)t.lo, (unsigned long long)((p->hi - p->lo) >> 20),
                 (unsigned long long)kb, after, t.base_ms, back, slow ? "they stay mirrored" : "they stay in guest memory");
        else
        logf("vk: pool %#llx (%llu MB): the CPU and the GPU both write it; in guest memory the GPU took %.2f ms a frame, %.2f "
             "before, %.2f mirrored again: %s", (unsigned long long)t.lo, (unsigned long long)((p->hi - p->lo) >> 20), after, t.base_ms,
             back, slow ? "mirrored" : "stays in guest memory");
        return finish();
    }
}

// ---- imports given back under pressure (16 GB PCs). The driver pins every imported pool and counts it in the system heap
// the GPU reaches, whose budget is about half the PC's RAM (7.5 GB of 16): imports were kept for the session, the heap
// went past its budget (7758 of 7503 MB on an RTX 3060 with 16 GB) and new imports failed with VK_ERROR_OUT_OF_DEVICE_MEMORY:
// terrain left out, load zones stuck. Past RX_VK_IMPORT_HIGH percent of the budget (85) the pools bound least lately give
// their imports back down to RX_VK_IMPORT_LOW percent (75): only pools without a mirror, not bound for kImportIdle
// submissions and done on the GPU. They leave the table as a retired pool does (the page table's entries 0, the caches
// emptied) and are imported again at their next binding; their memory is freed once everything recorded until now is
// done. On Linux, on by default only where that heap's budget is under 12 GB (where it ran out); off by default on Windows.
// RX_VK_IMPORT_HIGH=0: off, =85: on anywhere. (Caveat: last_value counts bindings, not shaders reaching a pool through the
// page table; a pool only reached that way would read zeros until it is bound again. Not seen in play: the pools given back
// were long unbound.) ----
bool system_heap_usage(uint64_t* used, uint64_t* budget);   // (vk_vma.cpp)
std::deque<std::map<uint64_t, Pool>::node_type> evicted_pools;
int64_t st_imports_evicted = 0;
uint64_t st_import_bytes_evicted = 0;
constexpr uint64_t kImportIdle = 600;

void imports_give_back() {   // (pools_mu held)
    while (!evicted_pools.empty() && evicted_pools.front().mapped().retired_value <= mem_completed) {
        Pool& p = evicted_pools.front().mapped();
        vkDestroyBuffer(dev.device, p.buffer, nullptr);
        vkFreeMemory(dev.device, p.memory, nullptr);
        for (VkDeviceMemory m : p.pieces) vkFreeMemory(dev.device, m, nullptr);   // (a pool imported as several dma-bufs)
        evicted_pools.pop_front();
    }
    // (hookshot) Off by default on Windows, as giving retired imports back is (RX_VK_KEEP_RETIRED, retired_imports_give_back:
    // device losses on Intel Arc and AMD right after imports went back): RX_VK_IMPORT_HIGH=85 turns it on there
    // (Triforce, 2026-11-11) On by default on Windows too where the memory handling is on (memory_trial_on: NVIDIA cards, which
    // have given retired imports back since issue 1057 with no loss, and the other makes on trial, where a device lost turns it
    // off again). An import is RAM the driver pins: 59 of them (2.0 GB) at a fresh Market, 201 (6.5 GB) after a tour of ten
    // areas, of ~17 GB the process holds in RAM by then. On a 16 GB PC that is what cannot be paged out while everything else is:
    // two players' logs there show a working set of 8-10 GB against peaks of 11.2-11.4, the system taking pages back all the
    // time, and on one the staging arenas refused ("no 64 MB arena could be made; the upload waits", 8,586 textures left for a
    // later frame: the flat white and blue ones).
    // And a second trigger beside the heap's budget: the PC's RAM itself running short (under 8% of it or 1.5 GB available,
    // GlobalMemoryStatusEx; RX_VK_IMPORT_RAM=0: not), which then gives back every import unbound for kImportIdleRam submissions
    // (~10 s of play), up to 1 GB a pass. With RAM to spare nothing changes: an import given back costs 5-45 ms when its pool is
    // bound again. RX_VK_IMPORT_HIGH=0: off altogether. (lab: RX_VK_TEST_AVAIL_MB=<n> plays that much RAM available)
    static const char* env_high = getenv("RX_VK_IMPORT_HIGH");
#ifdef _WIN32
    static const int high = env_high ? atoi(env_high) : (memory_trial_on() ? 85 : 0);
#else
    static const int high = env_high ? atoi(env_high) : 85;
#endif
    static const int low = getenv("RX_VK_IMPORT_LOW") ? std::clamp(atoi(getenv("RX_VK_IMPORT_LOW")), 10, 95) : 75;
    static uint64_t calls = 0;
    if (high <= 0 || ++calls % 16) return;   // (the driver asked every 16 submissions)
    uint64_t used = 0, budget = 0;
    if (!system_heap_usage(&used, &budget)) return;
    static const bool small = [&] {   // (on by default where the budget is small: 16 GB of RAM and less)
        const bool on = env_high || budget < (12ull << 30);
        logf("vk: imports given back past %d%% of a %llu MB system heap budget: %s", high, (unsigned long long)(budget >> 20),
             on ? "on" : "off (RX_VK_IMPORT_HIGH=85 turns it on)");
        return on;
    }();
    // (the RAM's trigger: asked every 256 submissions, ~0.7 s)
    static const bool ram_rule = !(getenv("RX_VK_IMPORT_RAM") && !atoi(getenv("RX_VK_IMPORT_RAM")));
    static const int64_t test_avail = getenv("RX_VK_TEST_AVAIL_MB") ? atoll(getenv("RX_VK_TEST_AVAIL_MB")) : -1;
    static bool ram_short = false;
    static uint64_t ram_avail = 0, ram_total = 0;
    if (ram_rule && calls % 256 == 0) {
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof ms;
        if (GlobalMemoryStatusEx(&ms)) {
            ram_total = ms.ullTotalPhys, ram_avail = test_avail >= 0 ? (uint64_t)test_avail << 20 : ms.ullAvailPhys;
            ram_short = ram_avail < std::max<uint64_t>(ram_total / 100 * 8, 1536ull << 20);
        }
    }
    const bool by_heap = small && used > budget / 100 * (uint64_t)high;
    if (!by_heap && !ram_short) return;
    const uint64_t target = budget / 100 * (uint64_t)low;
    const uint64_t want = by_heap ? (used > target ? used - target : 0) : (1ull << 30);
    constexpr uint64_t kImportIdleRam = 3600;
    const uint64_t idle_for = by_heap ? kImportIdle : kImportIdleRam;
    std::vector<std::map<uint64_t, Pool>::iterator> idle;
    for (auto it = imported.begin(); it != imported.end(); ++it) {
        const Pool& p = it->second;
        if ((p.mstate == Pool::MIRROR_NONE || p.mstate == Pool::MIRROR_REFUSED) && p.last_value <= mem_completed &&
            mem_rec_value > p.last_value + idle_for)
            idle.push_back(it);
    }
    std::sort(idle.begin(), idle.end(), [](auto a, auto b) { return a->second.last_value < b->second.last_value; });
    uint64_t freed = 0;
    int n = 0;
    for (auto it : idle) {
        if (freed >= want) break;
        Pool& p = it->second;
        guest_pages_set(p.lo, p.hi, 0);
        p.retired_value = mem_rec_value;   // (this submission's recording is being made: freed once it is done)
        freed += p.hi - p.lo;
        n++;
        evicted_pools.push_back(imported.extract(it));
    }
    if (!n) {
        static int logged_none;
        if (logged_none++ < 8)
            logf("vk: system heap %llu of %llu MB: no idle import to give back (%zu imported)", (unsigned long long)(used >> 20),
                 (unsigned long long)(budget >> 20), imported.size());
        return;
    }
    pools_gen++;
    last_pool = nullptr;
    std::fill(std::begin(pool_cache), std::end(pool_cache), nullptr);
    if (mirror_on()) publish_cap_ranges();
    st_imports_evicted += n, st_import_bytes_evicted += freed;
    static int logged;
    if (logged++ < 40 || logged % 64 == 0)
        logf("vk: system heap %llu of %llu MB%s: %d idle imports given back (%llu MB); %lld (%llu MB) in all",
             (unsigned long long)(used >> 20), (unsigned long long)(budget >> 20),
             by_heap ? "" : (std::string(", RAM short (") + std::to_string(ram_avail >> 20) + " of " + std::to_string(ram_total >> 20) + " MB available)").c_str(),
             n, (unsigned long long)(freed >> 20), (long long)st_imports_evicted, (unsigned long long)(st_import_bytes_evicted >> 20));
}

/// (Triforce) The memory of the mirrors' pieces made in the recording being made, bound to their sparse buffers: before the
/// recording is submitted (texture_uploads_recorded), waited for. A bind is not ordered with command buffers by their barriers,
/// but these pieces are new: nothing submitted before reads or writes them (the syncs and the bindings leave out what has no
/// memory), so the queue is not waited for
VkSemaphore piece_bind_tl = VK_NULL_HANDLE;   // (a timeline: the binds made so far)
std::atomic<uint64_t> piece_bind_value{0};
/// The wait a queue submission makes for the mirrors' piece binds sent so far (false: none sent, nothing to wait for). A value
/// reached long ago costs the queue nothing
bool memory_mirror_bind_wait(VkSemaphoreSubmitInfo* wait) {
    const uint64_t v = piece_bind_value.load(std::memory_order_acquire);
    if (!v || !piece_bind_tl) return false;
    *wait = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait->semaphore = piece_bind_tl, wait->value = v, wait->stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    return true;
}
void memory_mirror_binds_flush() {
    static std::mutex flush_mu;   // (one flush at a time: the list's head is this flush's until it takes it off)
    static std::vector<PieceBind> take;
    {
        std::lock_guard lk(piece_binds_mu);
        if (piece_binds.empty()) return;
    }
    std::lock_guard fl(flush_mu);
    {
        std::lock_guard lk(piece_binds_mu);
        if (piece_binds.empty()) return;
        take.assign(piece_binds.begin(), piece_binds.end());
    }
    const auto t0 = std::chrono::steady_clock::now();
    std::stable_sort(take.begin(), take.end(), [](const PieceBind& a, const PieceBind& b) { return a.buffer < b.buffer; });
    std::vector<VkSparseMemoryBind> binds;
    binds.reserve(take.size());
    for (const PieceBind& b : take) binds.push_back(b.bind);
    std::vector<VkSparseBufferMemoryBindInfo> infos;
    for (size_t i = 0; i < take.size();) {
        size_t j = i;
        while (j < take.size() && take[j].buffer == take[i].buffer) j++;
        infos.push_back({take[i].buffer, (uint32_t)(j - i), binds.data() + i});
        i = j;
    }
    VkBindSparseInfo si{VK_STRUCTURE_TYPE_BIND_SPARSE_INFO};
    si.bufferBindCount = (uint32_t)infos.size();
    si.pBufferBinds = infos.data();
    // (Triforce, later that day) Not waited for here any more: the bind signals a timeline semaphore, and every recording
    // submitted from then on waits for it on the queue (memory_mirror_bind_wait: submit_recording and the batched and the
    // segments' submits). The wait on the recording thread was 5 to 30 ms a flush whenever the GPU was busy, and 80 to 185 ms at
    // an area's load (a player's session, 2026-11-11: 825 flushes, 1.5 s in all). RX_VK_PIECE_BIND_WAIT=1: waited for, as before
    static const bool wait_here = getenv("RX_VK_PIECE_BIND_WAIT") != nullptr;
    static VkFence fence = VK_NULL_HANDLE;
    if (wait_here && !fence) {
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vkCreateFence(dev.device, &fi, nullptr, &fence);
    }
    if (!wait_here && !piece_bind_tl) {
        VkSemaphoreTypeCreateInfo ti{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo tsi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        tsi.pNext = &ti;
        if (vkCreateSemaphore(dev.device, &tsi, nullptr, &piece_bind_tl) != VK_SUCCESS) piece_bind_tl = VK_NULL_HANDLE;
    }
    const bool by_semaphore = !wait_here && piece_bind_tl;
    if (!by_semaphore && !fence) {
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vkCreateFence(dev.device, &fi, nullptr, &fence);
    }
    const uint64_t value = piece_bind_value.load(std::memory_order_relaxed) + 1;
    VkTimelineSemaphoreSubmitInfo tl{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    tl.signalSemaphoreValueCount = 1, tl.pSignalSemaphoreValues = &value;
    if (by_semaphore) si.pNext = &tl, si.signalSemaphoreCount = 1, si.pSignalSemaphores = &piece_bind_tl;
    VkResult res;
    {
        std::lock_guard qk(dev.present_mu);
        queue_flush_locked();
        res = vkQueueBindSparse(dev.queue, 1, &si, by_semaphore ? VK_NULL_HANDLE : fence);
        if (res == VK_SUCCESS && by_semaphore) piece_bind_value.store(value, std::memory_order_release);   // (under the queue's lock: the submits after it wait)
    }
    if (res == VK_SUCCESS) {
        if (!by_semaphore) {
            vkWaitForFences(dev.device, 1, &fence, VK_TRUE, UINT64_MAX);
            vkResetFences(dev.device, 1, &fence);
        }
    } else {
        static int told;
        if (told++ < 8) logf("vk: mirrors in pieces: %zu binds failed (VkResult %d)", take.size(), (int)res);
        if (res == VK_ERROR_DEVICE_LOST) device_lost();
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    {   // (only now off the list: a mirror given back meanwhile keeps its buffer until its binds are made, reap_mirrors)
        std::lock_guard lk(piece_binds_mu);
        piece_binds.erase(piece_binds.begin(), piece_binds.begin() + (ptrdiff_t)std::min(take.size(), piece_binds.size()));
        st_piece_flushes++, st_piece_bind_ms += ms;
    }
    static int slow;
    if (ms > 4.0 && slow++ < 40) logf("vk: mirrors in pieces: %zu binds took %.1f ms", take.size(), ms);
}

void memory_begin_submission() {
    std::lock_guard lk(pools_mu);
    reap_mirrors();
    demote_trials();
    retired_imports_give_back();
    mirror_epoch++;
    mirror_upload = true;
    mirror_synced.clear();
    writebacks.clear();
    imports_give_back();
    if (tex_capture_on()) publish_tex_spans(texture_frame_no());
    if (!mirror_on()) return;
    {
        uint64_t want = 0;
        if (!mirror_pieces_on())   // (Triforce: in pieces a new mirror holds nothing until its pieces are bound)
            for (Pool* p : mirror_wanted) want += p->hi - p->lo;
        if (mirror_epoch % 64 == 0 || mirror_bytes + want > mirror_budget()) mirrors_give_back(want);
    }
    if (!piece_wanted.empty()) {   // (Triforce: the pieces a parallel replay's segments bound without memory, made now)
        for (const PieceWanted& w : piece_wanted) {
            const auto it = imported.find(w.lo);
            if (it != imported.end() && it->second.mstate == Pool::MIRROR_ON && it->second.mpieces && w.off < it->second.hi - it->second.lo)
                mirror_pieces_make(it->second, w.off, std::min(w.size, it->second.hi - it->second.lo - w.off), true);
        }
        piece_wanted.clear();
    }
    {   // (Triforce: mirrors in pieces, every 1200 submissions: what they hold)
        static uint64_t at;
        if (mirror_pieces_on() && mirror_epoch - at >= 1200) {
            at = mirror_epoch;
            uint64_t whole = 0, spans = 0, held = 0;
            size_t n_pieces = 0, n_whole = 0;
            for (auto& [lo, p] : imported) {
                if (p.mstate != Pool::MIRROR_ON) continue;
                if (p.mpieces) n_pieces++, spans += p.hi - p.lo, held += p.res_bytes;
                else n_whole++, whole += p.hi - p.lo;
            }
            logf("vk: mirrors in pieces: %zu mirrors hold %llu MB of the %llu MB their pools span; %zu whole mirrors (%llu MB); %lld "
                 "piece runs bound in %lld flushes (%.1f ms in all); %lld bindings given guest memory instead", n_pieces,
                 (unsigned long long)(held >> 20), (unsigned long long)(spans >> 20), n_whole, (unsigned long long)(whole >> 20),
                 (long long)st_piece_binds, (long long)st_piece_flushes, st_piece_bind_ms, (long long)st_piece_direct);
        }
    }
    if (mirror_wanted.empty()) return;
    for (Pool* p : mirror_wanted) mirror_make(*p);
    mirror_wanted.clear();
    publish_cap_ranges();
    static uint64_t logged_at;
    if (mirror_bytes >> 28 != logged_at) {
        logged_at = mirror_bytes >> 28;
        logf("vk: %lld pools mirrored in VRAM (%llu MB)", (long long)st_mirrors, (unsigned long long)(mirror_bytes >> 20));
    }
}

/// (hookshot) Before a command buffer's parallel segments: every mirrored pool synced once, into the replay thread's
/// recording, ahead of all of them. Workers record out of order, so a pool a later segment bound first was synced in
/// that segment's upload, which runs after the earlier segments' work, and put the CPU's pages back over what the GPU
/// had just written in them (one frame of wrong lighting or exposure: 5x the flashes of serial replay). A segment
/// finds its pools synced now (synced_epoch, sync_seg 0). RX_VK_NO_PRESYNC=1: as before
void memory_sync_mirrors_before_segments() {
    static const bool off = getenv("RX_VK_NO_PRESYNC") != nullptr;
    if (off || !mirror_on() || !mirror_upload) return;
    std::lock_guard lk(pools_mu);
    for (auto& kv : imported) {
        Pool& p = kv.second;
        if (p.mstate != Pool::MIRROR_ON || p.synced_epoch == mirror_epoch) continue;
        mirror_sync(p);
        p.sync_seg = 0;
    }
}

/// A pool's mirror made, filled in the recording's upload command buffer: all of it, then the pages a capture holds
/// as they were at their submit (pools_mu held). False over the budget (tried again when next bound, as mirrors are
/// given back) or over 1 GB (never)
bool mirror_make(Pool& pool) {
    Pool* p = &pool;
    {   // (Evil Hookshot, RX_VK_NO_MIRROR_POOLS=<lo hex>[,...]: those pools stay in guest memory, the GPU's writes there at once)
        static const std::vector<uint64_t> no_mirror = [] {
            std::vector<uint64_t> v;
            for (const char* q = getenv("RX_VK_NO_MIRROR_POOLS"); q && *q;) {
                char* end;
                const uint64_t a = strtoull(q, &end, 16);
                if (end == q) break;
                v.push_back(a);
                q = *end ? end + 1 : end;
            }
            return v;
        }();
        if (std::find(no_mirror.begin(), no_mirror.end(), p->lo) != no_mirror.end()) {
            p->mstate = Pool::MIRROR_REFUSED;
            logf("vk: pool %#llx stays in guest memory (RX_VK_NO_MIRROR_POOLS)", (unsigned long long)p->lo);
            return false;
        }
    }
    {
        const uint64_t n = p->hi - p->lo;
        // (hookshot: RX_VK_MIRROR_TIGHT=1, none while VRAM is 90% full. Off: 8 GB cards sit there all the time, and without their mirrors
        // the GPU read game memory over PCIe)
        static const bool tight_rule = getenv("RX_VK_MIRROR_TIGHT") != nullptr;
        uint64_t used = 0, allowed = 0;
        const bool tight = tight_rule && vram_usage(&used, &allowed) && allowed && used > allowed / 100 * 90;
        // (Triforce) in pieces: a sparse buffer with no memory yet (mirrors in pieces, above); its pieces count against the
        // budget as they are made. A pool of under four pieces gets a whole mirror
        static const uint64_t piece_cfg = [] {
            const uint64_t kb = getenv("RX_VK_MIRROR_PIECE_KB") ? (uint64_t)std::clamp(atoi(getenv("RX_VK_MIRROR_PIECE_KB")), 64, 16384) : 256;
            return std::bit_ceil(kb << 10);
        }();
        bool in_pieces = false;
        if (mirror_pieces_on() && n <= (1ull << 30) && !tight && n >= 4 * piece_cfg) {
            VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bi.flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT;
            bi.size = n;
            bi.usage = kUsage;
            VkBuffer sb = VK_NULL_HANDLE;
            if (vkCreateBuffer(dev.device, &bi, nullptr, &sb) == VK_SUCCESS) {
                VkMemoryRequirements req{};
                vkGetBufferMemoryRequirements(dev.device, sb, &req);
                const uint64_t piece = std::max<uint64_t>(piece_cfg, std::bit_ceil(std::max<uint64_t>(req.alignment, 4096)));
                if (req.alignment && std::has_single_bit((uint64_t)req.alignment) && req.memoryTypeBits && n >= 2 * piece) {
                    p->mirror = sb, p->mirror_alloc = nullptr;
                    p->mpieces = true;
                    p->piece_shift = (uint32_t)std::countr_zero(piece);
                    p->piece_align = req.alignment, p->piece_types = req.memoryTypeBits, p->mirror_size = req.size;
                    const size_t words = (size_t)((((n + piece - 1) >> p->piece_shift) + 63) / 64);
                    if (p->res_bits.size() != words) p->res_bits.assign(words, 0);   // (once: read without the lock)
                    if (p->direct_bits.size() != words) p->direct_bits.assign(words, 0);   // (kept from its mirror before, if any)
                    p->res_allocs.clear(), p->res_bytes = 0;
                    VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
                    da.buffer = sb;
                    p->mirror_address = vkGetBufferDeviceAddress(dev.device, &da);
                    in_pieces = true;
                    static int logged;
                    if (logged++ < 4)
                        logf("vk: pool %#llx (%llu MB): its mirror in pieces of %llu KB (sparse block %llu KB)", (unsigned long long)p->lo,
                             (unsigned long long)(n >> 20), (unsigned long long)(piece >> 10), (unsigned long long)(req.alignment >> 10));
                } else {
                    vkDestroyBuffer(dev.device, sb, nullptr);
                }
            }
        }
        std::vector<std::pair<uint64_t, uint64_t>> made;   // (in pieces: the parts given memory here, for the captures' pages)
        if (in_pieces) {
            // (the parts whose GPU contents a retired pool's mirror holds, below: memory under them first, filled from guest memory)
            for (auto& nh : retired_pools) {
                Pool& r = nh.mapped();
                if (!r.inherit_pending || r.mstate != Pool::MIRROR_ON || !r.mirror || r.hi <= p->lo || r.lo >= p->hi) continue;
                const uint64_t a = std::max(r.lo, p->lo), b = std::min(r.hi, p->hi);
                if (!r.mpieces) {
                    mirror_pieces_make(*p, a - p->lo, b - a, false, &made);
                    continue;
                }
                const uint64_t rp = 1ull << r.piece_shift;
                for (uint64_t at = a; at < b;) {   // (the old mirror's resident runs in [a, b))
                    const uint64_t next = std::min(b, r.lo + ((at - r.lo) / rp + 1) * rp);
                    if (piece_resident(&r, (at - r.lo) >> r.piece_shift)) mirror_pieces_make(*p, at - p->lo, next - at, false, &made);
                    at = next;
                }
            }
        } else
        if (mirror_bytes + n > mirror_budget() || n > (1ull << 30) || tight) {
            p->mstate = n > (1ull << 30) ? Pool::MIRROR_REFUSED : Pool::MIRROR_NONE;
            static int logged;
            if (logged++ < 8)
                logf("vk: pool %#llx (%llu MB) stays in system memory: the mirrors have %llu MB of %llu", (unsigned long long)p->lo,
                     (unsigned long long)(n >> 20), (unsigned long long)(mirror_bytes >> 20),
                     (unsigned long long)(mirror_budget() >> 20));
            return false;
        }
        if (!in_pieces) {
            p->mpieces = false;
            if (!create_device_buffer(n, kUsage, p->mirror, p->mirror_alloc)) {
                p->mstate = Pool::MIRROR_REFUSED;
                return false;
            }
            {   // (hookshot) the page table points at the mirror: what the GPU wrote is there, and guest memory has it only after the write-back
                VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
                da.buffer = p->mirror;
                const uint64_t ma = vkGetBufferDeviceAddress(dev.device, &da);
                guest_pages_set(p->lo, p->hi, ma);
                if (getenv("RX_VK_LOG_ADDR"))
                    logf("vk: addr mirror %#llx..%#llx at gpu %#llx..%#llx", (unsigned long long)p->lo, (unsigned long long)p->hi,
                         (unsigned long long)ma, (unsigned long long)(ma + n));
            }
            const VkBufferCopy all{0, 0, n};
            GprofSpan gp(upload_command_buffer(), "mmake copy (a whole pool -> its new mirror)");
            gp.bytes = n;
            vkCmdCopyBuffer(upload_command_buffer(), p->buffer, p->mirror, 1, &all);   // (all of it, at this generation)
        }
        {   // (EvilOOT) then the mirrors of the pools it took in, over their part: what the GPU wrote there and only there
            // (retire_pool). The captures' pages go over it after, as before: the CPU's newer writes win
            bool barrier_done = false;
            int inherited = 0;
            for (auto& nh : retired_pools) {
                Pool& r = nh.mapped();
                if (!r.inherit_pending || r.mstate != Pool::MIRROR_ON || !r.mirror || r.hi <= p->lo || r.lo >= p->hi) continue;
                if (!barrier_done) {   // (after the full copy, and after the shaders of earlier submissions wrote the old mirror)
                    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
                    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                    mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
                    mb.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                    mb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
                    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                    di.memoryBarrierCount = 1, di.pMemoryBarriers = &mb;
                    vkCmdPipelineBarrier2(upload_command_buffer(), &di);
                    barrier_done = true;
                }
                const uint64_t a = std::max(r.lo, p->lo), b = std::min(r.hi, p->hi);
                // (only the pages not stamped since its last sync: one the CPU wrote after it has the game's newer data in the
                // copy of guest memory above, and the old mirror's over it counted as synced, stale for good (mirror_inherit.h).
                // As mirror_sync, a page a shader wrote through a bare entry keeps the mirror's copy (RX_VK_PROTECT_BARE=1).
                // Synced in this submission: the whole part. Its synced_gen is raised only at the submission's end
                // (memory_end_submission), so what the GPU wrote in this submission's earlier segments (RX_VK_PARALLEL) is
                // stamped above it, and guest memory has that only after the write-back; the CPU's writes were collected
                // before the sync. RX_VK_INHERIT_ALL=1: the whole part always, as before)
                static const bool inherit_all = getenv("RX_VK_INHERIT_ALL") != nullptr;
                std::vector<VkBufferCopy> parts;
                uint64_t from_guest = 0;
                const uint32_t synced = r.synced_gen;
                if (inherit_all || r.synced_epoch == mirror_epoch)
                    parts.push_back({a - r.lo, a - p->lo, b - a});
                else
                    from_guest = mirror_inherit::runs(a, b, r.lo, p->lo, kPageShift, kRegionPages,
                                                      [&](uint64_t region) { return region_gen[region] <= synced; },
                                                      [&](uint64_t page) {
                                                          return page_gen(page) <= synced ||
                                                                 (protect_bare_on() && prot_page_since(page, synced));
                                                      },
                                                      [&](uint64_t src, uint64_t dst, uint64_t size) { parts.push_back({src, dst, size}); });
                // (Triforce: mirrors in pieces: only what the old one holds in memory, into what the new one has memory for)
                clip_resident(r, parts, true);
                clip_resident(*p, parts);
                if (!parts.empty())
                    vkCmdCopyBuffer(upload_command_buffer(), r.mirror, p->mirror, (uint32_t)parts.size(), parts.data());
                if (from_guest) {
                    static int told;
                    if (told++ < 40)
                        logf("vk: pool %#llx..%#llx: %llu KB the CPU wrote since retired pool %#llx's last sync kept from guest memory, "
                             "not its mirror", (unsigned long long)p->lo, (unsigned long long)p->hi, (unsigned long long)(from_guest >> 10),
                             (unsigned long long)r.lo);
                }
                r.inherit_pending = false;
                r.retired_value = std::max(r.retired_value, mem_rec_value);   // (in use by this upload until it is done)
                inherited++;
            }
            if (inherited) {
                static int logged;
                if (logged++ < 16)
                    logf("vk: pool %#llx..%#llx: mirror made with the GPU's contents of %d retired pool(s)", (unsigned long long)p->lo,
                         (unsigned long long)p->hi, inherited);
            }
        }
        // (then the pages a capture holds as they were at their submit: guest memory is the game's frame ahead. Triforce: in
        // pieces, over the parts made here; a piece made later takes its own at its making)
        if (!in_pieces) mirror_captures_over(p, 0, n);
        else
            for (const auto& [a, b] : made) mirror_captures_over(p, a, b);
        p->mstate = Pool::MIRROR_ON;
        pools_gen.fetch_add(1, std::memory_order_relaxed);   // (the binding's buffer changes: guest memory, then this mirror)
        p->synced_gen = gen_now;
        p->synced_epoch = mirror_epoch;
        p->sync_seg = texture_segment();
        mirror_synced.push_back(p);
        if (!in_pieces) mirror_bytes += n, st_sync_bytes += (int64_t)n;   // (in pieces: counted as they are made)
        st_mirrors++;
    }
    return true;
}

// ---- the write-back's diff (hookshot, 2026-11-09) ----
// A mirrored pool's write-back copied every written run whole from the mirror to guest memory: ~69 MB a frame in Hyrule Field
// (an i7-14700K and an RTX 5070 Ti), 4.4 ms of the GPU's ~13, at the end of the frame's last submission, which the pages of the
// next capture wait for (the capture's write-back wait above). The runs are the written bindings' whole ranges: ~82% of what
// they copied was zeros, and of the rest little changed from one frame to the next. Now a pool written back keeps a shadow in
// VRAM, what guest memory holds there as far as it is known, page by page. A compute pass (shaders/wb_diff.comp) copies the
// words of a run that differ from the shadow, into guest memory and the shadow both; a page the CPU wrote since (the captures'
// pages and the replay's own words, shadow_written) is read from guest memory into the shadow first. Guest memory comes out as
// with the whole copy, or, for words the GPU left as they were and the CPU wrote since, with the CPU's words, as on the console:
// the pass only ever writes fewer words than the copy did. The pass takes ~0.7 ms there, the GPU's frame went from ~13.1 to
// ~9.2 ms, and running there the game went from ~53.9 to ~57.9 fps with 4x frame interpolation (from ~55.2 to ~58.7 without).
// (Word by word it was far slower than the copies: the GPU's writes to system memory went at ~1 GB/s; a subgroup's whole span
// written at once goes at the copies' speed or better.)
// RX_VK_WB_DIFF=0: the whole copies, as before. RX_VK_WB_SHADOW_MB: the shadows' VRAM at most (default 1/8 of it)
// What it rests on: a shadow page it knows equals guest memory. So every CPU write of guest memory reaches shadow_written before
// the next write-back reads the list: the captures' pages (capture_make), every page the collection finds (memory_collect_written:
// all of the game's writes with RX_VK_CAPTURE=0), and the replay's own writes, through guest_cpu_write / guest_cpu_wrote: the
// in-stream clears of the counters the game's conditions read, the copies, clears and updates done on the CPU, the textures
// written back into their memory, WriteTexels, the report words (memory_cpu_owned). One the shadows never heard of left the
// CPU's value in guest memory where the whole copy wrote the GPU's, whenever the GPU's result equalled the shadow's old word:
// a counter cleared in the stream and counted to last frame's value again stayed cleared, and what it switched stayed off
// (PR 1109's review). The 600-submit line counts the pages each kind of report made the shadows read again.
// The GPU's own writes of guest memory past the mirror do the same: a written binding given the pool's guest buffer
// (gpu_buffer_bind outside a recording's uploads), and a whole copy in place of the pass, forget the shadow's pages (shadow_forget).
std::mutex shadow_written_mu;
std::vector<uint32_t> shadow_written_pages;   // (guest pages the CPU wrote: unknown to the shadows at the next write-back)
std::vector<uint32_t> shadow_written_collected_list;   // (the same, of the collection: memory_collect_written)
std::vector<uint32_t> shadow_written_by_replay;   // (the same, of the replay's own CPU writes: guest_cpu_wrote)
bool shadow_written_all = false;              // (more than those lists hold: every page unknown at the next write-back)
int64_t st_wbd_known_game = 0, st_wbd_known_collected = 0, st_wbd_known_replay = 0;   // (pages the shadows knew when reported)
struct DeadShadow {
    VkBuffer buffer;
    VmaAllocation alloc;
    uint64_t value;
};
std::deque<DeadShadow> dead_shadows;
uint64_t shadow_bytes = 0;
int64_t st_wbd_runs = 0, st_wbd_bytes = 0, st_wbd_fill_pages = 0;
bool wb_diff_on() {
    // (hookshot) On by default again (v2.0.11). v2.0.9 turned it off with the presents' own queue after the device lost on an RTX 4090
    // with NVIDIA's 621 driver (issue 1161); the reporter's A/B then put that on the present queue (black screen with it alone), and the
    // write-back diff alone ran there and on drivers 615 and 621 for others: ~20% fps (an RTX 3080 laptop went 40 -> 25 fps without
    // it). RX_VK_WB_DIFF=0: off
    // (integrated GPUs too: an Ally X's hard reboots stopped with v2.0.8 and came back with v2.0.9's off, issue 997: the whole copies
    // are long submissions there. Its shadows are the system's RAM on them, so at most 512 MB: shadow_room)
    static const bool on = !getenv("RX_VK_WB_DIFF") || atoi(getenv("RX_VK_WB_DIFF")) != 0;
    return on;
}
void shadow_free(Pool& p) {   // (pools_mu held: destroyed once the submissions so far are done)
    if (!p.shadow) return;
    dead_shadows.push_back({p.shadow, p.shadow_alloc, std::max(mem_rec_value, p.last_value)});
    shadow_bytes -= p.hi - p.lo;
    p.shadow = VK_NULL_HANDLE, p.shadow_alloc = nullptr, p.shadow_address = 0;
    p.shadow_known.clear();
}
void shadows_reap() {
    while (!dead_shadows.empty() && dead_shadows.front().value <= mem_completed) {
        destroy_buffer(dead_shadows.front().buffer, dead_shadows.front().alloc);
        dead_shadows.pop_front();
    }
}
// (hookshot) The shadows' two rules read what is really allocated (the driver's count less the free space inside VMA's blocks,
// as the texture budget's 90% rule does: vram_pressure.h), not the count alone. By the count alone they went at 95% and
// came back only under 85%. On a 12 GB card the texture cache fills VRAM to 93-98% of what the driver allows within minutes
// of play, gigabytes of it free space inside the blocks that evicting cannot give back, so the shadows went (272-448 MB) and
// never returned: the write-backs were whole copies again for the rest of the session. In a player's log (RTX 4070 SUPER,
// 2560 x 1440, 10,462 of 10,732 MB used, 3.5 GB of it free inside the blocks) "the shadows given back" is where 58 fps
// became 26. Here, on v2.0.11's code with 9.8 GB allowed, five minutes across Hyrule Field, two runs each: 56.6 and 58.8 fps
// with 32 and 8 of 120 two-second windows under 55 fps before, 59.8 and 59.6 fps with 1 and 2 after; the GPU's frame
// 11.7-12.0 -> 9.8-10.1 ms, the capture's wait in a frame 2.1-2.4 -> 1.1 ms. (The free space is of no use to the shadows
// themselves, which are allocations of their own: they are made only under the textures' lines. vram_pressure.h)
// RX_VK_SHADOW_RAW=1: by the count alone, as before
bool shadow_rules_raw() {
    static const bool v = getenv("RX_VK_SHADOW_RAW") != nullptr;
    return v;
}
bool shadow_room(uint64_t n) {   // (the shadows' budget, and room in VRAM: the textures and the mirrors come first)
    static const uint64_t cap = [] {
        if (const char* e = getenv("RX_VK_WB_SHADOW_MB")) return (uint64_t)atoll(e) << 20;
        uint64_t used = 0, allowed = 0;
        const uint64_t eighth = vram_usage(&used, &allowed) && allowed ? allowed / 8 : (uint64_t)512 << 20;   // (unsigned long on Linux)
        // (an integrated GPU's VRAM is the system's RAM: 512 MB at most there)
        return dev.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? std::min<uint64_t>(eighth, (uint64_t)512 << 20) : eighth;
    }();
    if (shadow_bytes + n > cap) return false;
    uint64_t used = 0, allowed = 0, block_free = 0;
    if (!vram_usage(&used, &allowed, &block_free) || !allowed) return true;
    if (shadow_rules_raw()) return used + n <= allowed / 100 * 85;
    return vram_pressure::shadow_fits(n, used, allowed, block_free);
}
bool shadow_make(Pool& p) {
    const uint64_t n = p.hi - p.lo;
    if (!p.mirror || !shadow_room(n)) return false;
    if (!create_device_buffer(n, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, p.shadow, p.shadow_alloc))
        return false;
    VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    da.buffer = p.shadow;
    p.shadow_address = vkGetBufferDeviceAddress(dev.device, &da);
    p.shadow_known.assign((size_t)((n >> kPageShift) + 63) / 64, 0);
    shadow_bytes += n;
    static int logged;
    if (logged++ < 16)
        logf("vk: write-back diff: a shadow for pool %#llx (%llu MB; %llu MB of shadows)", (unsigned long long)p.lo,
             (unsigned long long)(n >> 20), (unsigned long long)(shadow_bytes >> 20));
    return true;
}
// (pools_mu held) every shadow given back while VRAM is really 95% full, or the driver's count at 99% (every 120 write-back
// submissions): made again when there is room
void shadows_check_room() {
    static int64_t n;
    if (!shadow_bytes || ++n % 120) return;
    uint64_t used = 0, allowed = 0, block_free = 0;
    if (!vram_usage(&used, &allowed, &block_free) || !allowed) return;
    if (shadow_rules_raw() ? used <= allowed / 100 * 95 : !vram_pressure::shadows_go(used, allowed, block_free)) return;
    const uint64_t had = shadow_bytes;
    for (auto& [lo, p] : imported) shadow_free(p);
    for (auto& nh : retired_pools) shadow_free(nh.mapped());
    static int logged;
    if (logged++ < 8)
        logf("vk: write-back diff: VRAM %llu of %llu MB: the shadows given back (%llu MB)", (unsigned long long)(used >> 20),
             (unsigned long long)(allowed >> 20), (unsigned long long)(had >> 20));
}
// (pools_mu held) a range of the pool (offsets) unknown to its shadow again
void shadow_forget(Pool& p, uint64_t at, uint64_t len) {
    if (!p.shadow || !len) return;
    for (uint64_t pg = at >> kPageShift; pg <= (at + len - 1) >> kPageShift; pg++) p.shadow_known[pg >> 6] &= ~(1ull << (pg & 63));
}
// (pools_mu held) the pages written since: unknown to their pools' shadows
void shadows_forget_written() {
    static thread_local std::vector<uint32_t> game, collected, replay;
    bool all;
    {
        std::lock_guard lk(shadow_written_mu);
        game.clear(), collected.clear(), replay.clear();
        game.swap(shadow_written_pages);
        collected.swap(shadow_written_collected_list);
        replay.swap(shadow_written_by_replay);
        all = shadow_written_all, shadow_written_all = false;
    }
    if (all)   // (written for a long while with no write-back to read the list: nothing of guest memory is known)
        for (auto& [lo, pool] : imported)
            if (pool.shadow) std::fill(pool.shadow_known.begin(), pool.shadow_known.end(), 0ull);
    if (!shadow_bytes) return;
    // (in this order, each page counted under the first that reports it: the collection's and the replay's counts are what the
    // captures alone would have left known)
    for (int which = 0; which < 3; which++) {
        std::vector<uint32_t>& pages = which == 0 ? game : which == 1 ? collected : replay;
        int64_t& known = which == 0 ? st_wbd_known_game : which == 1 ? st_wbd_known_collected : st_wbd_known_replay;
        if (pages.empty()) continue;
        std::sort(pages.begin(), pages.end());
        Pool* p = nullptr;
        for (uint32_t pg : pages) {
            const uint64_t a = (uint64_t)pg << kPageShift;
            if (!p || a < p->lo || a >= p->hi) {
                p = nullptr;
                auto it = imported.upper_bound(a);
                if (it == imported.begin()) continue;
                --it;
                if (a < it->second.hi) p = &it->second;
                if (!p) continue;
            }
            if (!p->shadow) continue;
            const uint64_t i = (a - p->lo) >> kPageShift;
            uint64_t& bits = p->shadow_known[i >> 6];
            if (bits & (1ull << (i & 63))) known++;
            bits &= ~(1ull << (i & 63));
        }
    }
}
// (the pass's pipeline: 16 bytes of push constants and no descriptors, its buffers by address; made once)
struct WbdPipe {
    bool tried = false;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
} wbd_pipe;
bool wbd_pipe_ready() {
    if (wbd_pipe.tried) return wbd_pipe.pipe != VK_NULL_HANDLE;
    wbd_pipe.tried = true;
    // (the pass votes across a subgroup (shaders/wb_diff.comp): a GPU without votes in compute keeps the whole copies)
    VkPhysicalDeviceSubgroupProperties sg{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &sg;
    vkGetPhysicalDeviceProperties2(dev.physical, &p2);
    if (!(sg.supportedOperations & VK_SUBGROUP_FEATURE_VOTE_BIT) || !(sg.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT)) {
        logf("vk: write-back diff off: the GPU has no subgroup votes in compute shaders");
        return false;
    }
    const VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &pr;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(dev.device, &li, nullptr, &wbd_pipe.layout) == VK_SUCCESS) {
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = sizeof WB_DIFF_COMP_SPV;
        mi.pCode = WB_DIFF_COMP_SPV;
        if (vkCreateShaderModule(dev.device, &mi, nullptr, &m) == VK_SUCCESS) {
            VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            ci.stage.module = m;
            ci.stage.pName = "main";
            ci.layout = wbd_pipe.layout;
            if (vkCreateComputePipelines(dev.device, VK_NULL_HANDLE, 1, &ci, nullptr, &wbd_pipe.pipe) != VK_SUCCESS)
                wbd_pipe.pipe = VK_NULL_HANDLE;
            vkDestroyShaderModule(dev.device, m, nullptr);
        }
    }
    const bool ok = wbd_pipe.pipe != VK_NULL_HANDLE;
    logf("vk: write-back diff %s", ok ? "on (RX_VK_WB_DIFF=0: off)" : "off: its pipeline was not made");
    return ok;
}
void shadow_written_to(std::vector<uint32_t>& list, const uint32_t* pages, int64_t n) {
    if (n <= 0 || !wb_diff_on()) return;
    std::lock_guard lk(shadow_written_mu);
    if (shadow_written_pages.size() + shadow_written_collected_list.size() + shadow_written_by_replay.size() > (8u << 20)) {
        shadow_written_all = true;   // (no write-back has read them for a long while: all of them, then)
        shadow_written_pages.clear(), shadow_written_collected_list.clear(), shadow_written_by_replay.clear();
        return;
    }
    list.insert(list.end(), pages, pages + n);
}
void shadow_written(const uint32_t* pages, int64_t n) {
    shadow_written_to(shadow_written_pages, pages, n);
}
void shadow_written_collected(const uint32_t* pages, int64_t n) {
    shadow_written_to(shadow_written_collected_list, pages, n);
}
int guest_cpu_write(uint64_t gpu, const void* src, uint64_t n) {
    const int ok = host->gpu_write(gpu, src, n);
    guest_cpu_wrote(gpu, n);
    return ok;
}
void guest_cpu_wrote(uint64_t gpu, uint64_t n) {
    if (!n || !wb_diff_on()) return;
    uint32_t pages[64];
    int k = 0;
    for (uint64_t a = gpu & ~4095ull; a < gpu + n; a += 4096) {   // (GPU pages are mapped to guest memory page by page)
        const uint64_t g = host->gpu_guest(a);
        if (!g) continue;
        pages[k++] = (uint32_t)(g >> kPageShift);
        if (k == 64) shadow_written_to(shadow_written_by_replay, pages, k), k = 0;
    }
    if (k) shadow_written_to(shadow_written_by_replay, pages, k);
}
// A run of a pool (offsets, in whole words) as the pass's chunks, in order: the words before its first 16-byte vector, the
// vectors 64 KB at most at a time, the words after the last; emit(offset, bytes, vectors). (tests/test_wb_diff_split.py)
template <class F> void wbd_split(uint64_t at, uint64_t size, F&& emit) {
    const uint64_t end = at + size;
    const uint64_t body = std::min<uint64_t>(end, (at + 15) & ~(uint64_t)15), body_end = std::max<uint64_t>(body, end & ~(uint64_t)15);
    if (body > at) emit(at, body - at, false);
    for (uint64_t a = body; a < body_end;) {
        const uint64_t len = std::min<uint64_t>(body_end - a, 64u << 10);
        emit(a, len, true);
        a += len;
    }
    if (end > body_end) emit(body_end, end - body_end, false);
}
struct WbdChunk {   // (shaders/wb_diff.comp's Chunk)
    uint64_t mirror, shadow, guest;
    uint32_t count, vecs;   // (16-byte vectors when vecs is 1, else words)
};
// (the pass's tables, mapped, a ring: one is taken again once the submission that read it is done)
struct WbdTable {
    VkBuffer buf = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    uint8_t* map = nullptr;
    VkDeviceAddress address = 0;
    uint64_t bytes = 0, value = 0;
};
WbdTable wbd_tables[64];
int wbd_table_next = 0;
WbdTable* wbd_table_get(uint64_t bytes) {
    WbdTable& t = wbd_tables[wbd_table_next];
    if (t.value > mem_completed) return nullptr;   // (still read by a submission the GPU has not done)
    if (t.bytes < bytes) {
        if (t.buf) destroy_buffer(t.buf, t.alloc);
        t = {};
        const uint64_t n = std::max<uint64_t>(128u << 10, (bytes + 65535) & ~65535ull);
        void* map = nullptr;
        if (!create_buffer(n, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true, t.buf, t.alloc, &map) ||
            !map) {
            if (t.buf) destroy_buffer(t.buf, t.alloc);
            t = {};
            return nullptr;
        }
        VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        da.buffer = t.buf;
        t.address = vkGetBufferDeviceAddress(dev.device, &da);
        t.map = (uint8_t*)map;
        t.bytes = n;
    }
    wbd_table_next = (wbd_table_next + 1) % 64;
    t.value = mem_rec_value;
    return &t;
}

void memory_end_submission(VkCommandBuffer cmd) {
    std::lock_guard lk(pools_mu);
    {   // (every 600 submissions: how the mirror syncs' dirty pages were read)
        static int64_t n, l_live, l_cap;
        if (++n % 600 == 0) {
            static int64_t l_old;
            logf("vk: mirror syncs (600 submits): %lld dirty pages from their submits' captures, %lld from earlier ones', "
                 "%lld from guest memory as it was at the replay; %zu captures kept; %lld pages the GPU wrote copied over "
                 "(%lld live); %lld kept for the GPU (RX_VK_PROTECT_BARE)", (long long)(st_sync_cap_pages - l_cap),
                 (long long)(st_sync_old_pages - l_old), (long long)(st_sync_live_pages - l_live), cap_bufs.size(),
                 (long long)st_sync_gpu_pages, (long long)st_sync_gpu_live, (long long)st_prot_kept.load());
            l_cap = st_sync_cap_pages, l_live = st_sync_live_pages, l_old = st_sync_old_pages;
            if (shadow_bytes || st_wbd_runs)
                logf("vk: write-back diff (600 submits): %lld runs, %.1f MB compared; %lld pages read into the shadows; %llu MB of shadows; "
                     "pages the shadows knew that the CPU wrote: %lld in captures, %lld only in the collection, %lld only by the replay itself",
                     (long long)st_wbd_runs, st_wbd_bytes / 1048576.0, (long long)st_wbd_fill_pages, (unsigned long long)(shadow_bytes >> 20),
                     (long long)st_wbd_known_game, (long long)st_wbd_known_collected, (long long)st_wbd_known_replay);
            st_wbd_runs = st_wbd_bytes = st_wbd_fill_pages = 0;
            st_wbd_known_game = st_wbd_known_collected = st_wbd_known_replay = 0;
            std::vector<std::pair<int64_t, uint64_t>> top;
            for (const auto& [lo, k] : live_by_pool) top.push_back({k, lo});
            std::sort(top.rbegin(), top.rend());
            std::string s;
            for (size_t i = 0; i < top.size() && i < 8; i++) {
                const auto it = imported.find(top[i].second);
                char b[160];
                bool captured = false;
                {
                    std::lock_guard ck(cap_ranges_mu);
                    for (const auto& r : cap_ranges) captured |= top[i].second >= r.first && top[i].second < r.second;
                }
                snprintf(b, sizeof b, " %#llx (%llu MB%s, state %d): %lld;", (unsigned long long)top[i].second,
                         it != imported.end() ? (unsigned long long)((it->second.hi - it->second.lo) >> 20) : 0ull,
                         captured ? ", captured" : ", NOT captured", it != imported.end() ? (int)it->second.mstate : -1,
                         (long long)top[i].first);
                s += b;
            }
            logf("vk:   read live, by pool:%s", s.c_str());
            live_by_pool.clear();
            // (and the bindings the GPU read in guest memory itself: pools with no mirror yet, or none (state 9: bound
            // outside a recording))
            std::vector<std::pair<int64_t, uint64_t>> dtop;
            for (const auto& [lo, e] : direct_by_pool) dtop.push_back({e.first, lo});
            std::sort(dtop.rbegin(), dtop.rend());
            std::string ds;
            for (size_t i = 0; i < dtop.size() && i < 8; i++) {
                const auto it = imported.find(dtop[i].second);
                char b[160];
                snprintf(b, sizeof b, " %#llx (%llu MB, state %d): %lld binds, %.0f MB;", (unsigned long long)dtop[i].second,
                         it != imported.end() ? (unsigned long long)((it->second.hi - it->second.lo) >> 20) : 0ull,
                         direct_by_pool[dtop[i].second].second, (long long)dtop[i].first,
                         direct_bytes_by_pool[dtop[i].second] / 1048576.0);
                ds += b;
            }
            if (!dtop.empty()) {
                double all = 0;
                for (const auto& [lo, bytes] : direct_bytes_by_pool) all += (double)bytes;
                logf("vk:   bound in guest memory, by pool (the GPU reads these over PCIe; binds and the bytes of their ranges over these 600 submits, "
                     "%.1f MB in all, a range counted once a submit): %s", all / 1048576.0, ds.c_str());
            }
            direct_by_pool.clear(), direct_bytes_by_pool.clear();
        }
    }
    static const bool no_writeback = getenv("RX_VK_NO_WRITEBACK") != nullptr;   // (an experiment: none)
    if (no_writeback) writebacks.clear();
    if (!writebacks.empty()) {
        // per pool, the ranges merged, one copy each (mirror -> the guest's pool)
        std::sort(writebacks.begin(), writebacks.end(), [](const WriteBack& a, const WriteBack& b) {
            return a.pool != b.pool ? a.pool < b.pool : a.lo < b.lo;
        });
        GprofSpan gp(cmd, "wb copies (mirror -> guest memory, with its two barriers)");   // (RX_VK_PASS_TIMING)
        gp.items = 0;
        transfer_barrier(cmd);
        std::vector<VkBufferCopy> runs;
        // (the write-back's diff: the runs of pools with a shadow, as chunks of 64 KB at most, for one pass after the loop)
        const bool diff = wb_diff_on() && wbd_pipe_ready();
        if (diff) shadows_forget_written(), shadows_reap(), shadows_check_room();
        static thread_local std::vector<WbdChunk> chunks;
        struct ChunkSrc { Pool* p; uint64_t at, len; };
        static thread_local std::vector<ChunkSrc> chunk_src;   // (for the whole copy, when no table is free)
        static thread_local std::vector<VkBufferCopy> fills, odd;
        chunks.clear(), chunk_src.clear();
        bool filled = false;
        for (size_t i = 0; i < writebacks.size();) {
            Pool* p = writebacks[i].pool;
            runs.clear();
            // (Triforce) the merged runs made of event words alone (memory_bind_event_write): written back like any other, not
            // noted for the captures' wait below
            static thread_local std::vector<std::pair<uint64_t, uint64_t>> event_runs;
            static thread_local std::vector<uint8_t> run_event;
            event_runs.clear(), run_event.clear();
            for (; i < writebacks.size() && writebacks[i].pool == p; i++) {
                const WriteBack& w = writebacks[i];
                if (!runs.empty() && w.lo <= runs.back().srcOffset + runs.back().size) {
                    runs.back().size = std::max(runs.back().size, w.hi - runs.back().srcOffset);
                    run_event.back() &= (uint8_t)w.event;
                } else {
                    runs.push_back({w.lo, w.lo, w.hi - w.lo});
                    run_event.push_back((uint8_t)w.event);
                }
            }
            for (size_t r = 0; r < runs.size(); r++)
                if (run_event[r]) event_runs.push_back({runs[r].dstOffset, runs[r].dstOffset + runs[r].size});
            cut_cpu_words(*p, runs);
            clip_resident(*p, runs);   // (Triforce: a mirror in pieces: written bindings have their pieces; nothing read where it has none)
            if (runs.empty()) continue;
            checkpoint_mark(cmd, p->lo, 'b');
            uint64_t run_bytes = 0;
            for (const VkBufferCopy& r : runs) run_bytes += r.size;
            if (diff && !p->shadow && run_bytes >= (256u << 10)) shadow_make(*p);   // (a pool whose write-back is worth one, as VRAM allows)
            if (diff && p->shadow && p->address && p->mirror) {
                VkBufferDeviceAddressInfo mda{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
                mda.buffer = p->mirror;
                const VkDeviceAddress mirror_address = vkGetBufferDeviceAddress(dev.device, &mda);
                // (a run not in whole words, as a copy of the game's may be: copied whole, its pages then unknown to the shadow)
                odd.clear();
                for (const VkBufferCopy& r : runs)
                    if ((r.dstOffset | r.size) & 3) odd.push_back(r);
                // the pages of the runs the shadow does not know: from guest memory, whole, first
                fills.clear();
                const uint64_t pool_n = p->hi - p->lo;
                for (const VkBufferCopy& r : runs)
                    for (uint64_t pg = r.dstOffset >> kPageShift; !((r.dstOffset | r.size) & 3) && pg <= (r.dstOffset + r.size - 1) >> kPageShift; pg++) {
                        uint64_t& bits = p->shadow_known[pg >> 6];
                        if (bits & (1ull << (pg & 63))) continue;
                        bits |= 1ull << (pg & 63);
                        const uint64_t at = pg << kPageShift, len = std::min<uint64_t>(4096, pool_n - at);
                        if (!fills.empty() && fills.back().srcOffset + fills.back().size == at)
                            fills.back().size += len;
                        else
                            fills.push_back({at, at, len});
                        st_wbd_fill_pages++;
                    }
                if (!fills.empty()) {
                    vkCmdCopyBuffer(cmd, p->buffer, p->shadow, (uint32_t)fills.size(), fills.data());
                    filled = true;
                }
                if (!odd.empty()) {
                    vkCmdCopyBuffer(cmd, p->mirror, p->buffer, (uint32_t)odd.size(), odd.data());
                    for (const VkBufferCopy& r : odd) shadow_forget(*p, r.dstOffset, r.size);
                }
                for (const VkBufferCopy& r : runs) {
                    if ((r.dstOffset | r.size) & 3) continue;
                    wbd_split(r.dstOffset, r.size, [&](uint64_t at, uint64_t len, bool vecs) {
                        chunks.push_back({mirror_address + at, p->shadow_address + at, p->address + at,
                                          (uint32_t)(len / (vecs ? 16 : 4)), vecs ? 1u : 0u});
                        chunk_src.push_back({p, at, len});
                    });
                }
                st_wbd_runs += (int64_t)runs.size(), st_wbd_bytes += (int64_t)run_bytes;
            } else {
                vkCmdCopyBuffer(cmd, p->mirror, p->buffer, (uint32_t)runs.size(), runs.data());
            }
            for (const VkBufferCopy& r : runs) st_wb_bytes += (int64_t)r.size, gp.bytes += r.size;
            gp.items += (int)runs.size();
            {   // (diagnostics) RX_VK_LOG_WB_RUNS=<n>: n submissions' write-back runs of at most 64 KB, after 3000 submissions
                static const int log_runs = getenv("RX_VK_LOG_WB_RUNS") ? atoi(getenv("RX_VK_LOG_WB_RUNS")) : 0;
                static int64_t subs_seen;
                static uint64_t last_value;
                if (log_runs) {
                    (void)subs_seen;
                    static const uint64_t from = getenv("RX_VK_LOG_WB_FROM") ? (uint64_t)atoll(getenv("RX_VK_LOG_WB_FROM")) : 3000;
                    if (mem_rec_value > from && !last_value) last_value = mem_rec_value;   // (the first value past it)
                    if (last_value && mem_rec_value < last_value + (uint64_t)log_runs)
                        for (const VkBufferCopy& r : runs)
                            if (r.size <= 65536)
                                logf("vk: wbrun: value %llu pool %#llx: %#llx +%#llx", (unsigned long long)mem_rec_value,
                                     (unsigned long long)p->lo, (unsigned long long)(p->lo + r.dstOffset), (unsigned long long)r.size);
                }
            }
            for (const VkBufferCopy& r : runs) {   // (EvilBoyce; Triforce: not the runs of event words alone, above)
                bool event_only = false;
                for (const auto& e : event_runs)
                    if (r.dstOffset >= e.first && r.dstOffset + r.size <= e.second) { event_only = true; break; }
                if (event_only) { st_wb_event_runs++; continue; }
                wb_pages_note(p->lo + r.dstOffset, p->lo + r.dstOffset + r.size, mem_rec_value);
            }
            {   // (hookshot) RX_VK_LOG_WRITEBACK=1: the pools written back the most, every 600 write-back submissions
                static const bool log_wb = getenv("RX_VK_LOG_WRITEBACK") != nullptr;
                if (log_wb) {
                    static std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> by_pool;   // lo -> bytes, runs
                    static int64_t subs;
                    auto& e = by_pool[p->lo];
                    for (const VkBufferCopy& r : runs) e.first += r.size;
                    e.second += runs.size();
                    if (i == writebacks.size() && ++subs % 600 == 0) {
                        std::vector<std::pair<uint64_t, std::pair<uint64_t, uint64_t>>> v(by_pool.begin(), by_pool.end());
                        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
                        std::string s;
                        for (size_t k = 0; k < v.size() && k < 8; k++) {
                            char b[120];
                            const auto it = imported.find(v[k].first);
                            snprintf(b, sizeof b, " %#llx (%llu MB pool): %.1f MB in %.0f runs;", (unsigned long long)v[k].first,
                                     it != imported.end() ? (unsigned long long)((it->second.hi - it->second.lo) >> 20) : 0ull,
                                     v[k].second.first / 600.0 / 1048576.0, v[k].second.second / 600.0);
                            s += b;
                        }
                        logf("vk: write-back a submission, by pool:%s", s.c_str());
                        by_pool.clear();
                    }
                }
            }
        }
        if (!chunks.empty()) {
            WbdTable* t = wbd_table_get(chunks.size() * sizeof(WbdChunk));
            if (!t) {   // (no table free: the whole copies, the shadows forgotten for those pages)
                for (const ChunkSrc& c : chunk_src) {
                    const VkBufferCopy r{c.at, c.at, c.len};
                    vkCmdCopyBuffer(cmd, c.p->mirror, c.p->buffer, 1, &r);
                    shadow_forget(*c.p, c.at, c.len);
                }
                static int logged;
                if (logged++ < 8) logf("vk: write-back diff: no chunk table free (%zu chunks): this one copied whole", chunks.size());
            }
            if (filled) transfer_barrier(cmd);   // (the shadows' pages from guest memory before the pass reads them)
            if (t) {
                memcpy(t->map, chunks.data(), chunks.size() * sizeof(WbdChunk));
                flush_mapped(t->alloc, chunks.size() * sizeof(WbdChunk));
                GprofSpan gd(cmd, "wb diff (mirror -> guest memory where it differs from the shadow)");
                gd.items = (int)chunks.size();
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, wbd_pipe.pipe);
                const struct { uint64_t table; uint32_t count, pad; } pc{t->address, (uint32_t)chunks.size(), 0};
                vkCmdPushConstants(cmd, wbd_pipe.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
                const uint32_t n = (uint32_t)chunks.size();
                vkCmdDispatch(cmd, std::min<uint32_t>(n, 65535u), (n + 65534u) / 65535u, 1);
            }
        }
        transfer_barrier(cmd);
        writebacks.clear();
    }
    // (what this submission synced is current as of now: the GPU's own writes, stamped while it was recorded, are
    // in the mirror and written back; CPU writes collected at the next submission get a newer generation)
    for (Pool* p : mirror_synced) p->synced_gen = gen_now;
    mirror_synced.clear();
    mirror_upload = false;
}

void memory_stats(int64_t* mirrors, uint64_t* mirror_mb, int64_t* sync_bytes, int64_t* sync_copies, int64_t* wb_bytes) {
    *mirrors = st_mirrors, *mirror_mb = mirror_bytes >> 20, *sync_bytes = st_sync_bytes, *sync_copies = st_sync_copies;
    *wb_bytes = st_wb_bytes;
}
} // namespace mkwvk
