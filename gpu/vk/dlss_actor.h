// (hookshot) The temporal upscaler's own motion (vk_temporal.cpp): what is worked out on the CPU for a draw drawn again, from
// its bones (actor_motion) or from the positions its vertex stage stored (capture_motion), kept apart so
// tests/test_dlss_actor.cpp checks it without a GPU. Header only, no Vulkan.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace dlss_actor {

/// A draw's name across frames: its model (vertex and index buffers, count, base vertex, program) and which of the frame's
/// draws of that model it is (the same model drawn twice: the n-th draw of it is matched with the n-th the frame before)
inline uint64_t key(uint64_t vbuf, uint64_t ibuf, int count, int base_vertex, uint64_t program, int nth) {
    uint64_t h = 0x5bd1e995;
    for (uint64_t v : {vbuf, ibuf, (uint64_t)(uint32_t)count, (uint64_t)(uint32_t)base_vertex, program}) {
        h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        h *= 0xff51afd7ed558ccdull;
    }
    return (h & ~0xffull) | (uint64_t)std::clamp(nth, 0, 255);
}
inline bool same_model(uint64_t a, uint64_t b) { return a >> 8 == b >> 8; }

/// shaders/dlss_actor.vert's push constants (the fragment stage's the same)
struct Push {
    uint64_t vertices;   // (the vertex stream's device address)
    uint64_t indices;    // (the index buffer's, 4-aligned down: first says where the draw's indices start in it)
    uint64_t origins[2]; // (the draw's origin, xyz: this frame's and the frame before's, copied into the pose buffers)
    uint64_t bones[2];   // (its palettes: 3 x 4 row-major matrices, 12 floats a bone)
    uint32_t nbones;
    uint32_t stride;     // (in words)
    uint32_t count;      // (the vertices in the stream: an index past them draws nothing)
    uint32_t first;      // (in indices, from the address)
    uint32_t itype;      // (0: 8-bit, 1: 16-bit, 2: 32-bit indices)
    int32_t base_vertex;
    uint32_t pos, sets;  // (bytes in a vertex: the position; the joint and weight sets, 1 or 2)
    uint32_t joints[2], weights[2];
    uint32_t formats;    // (kHalfPosition, kByteJoints << set)
    uint32_t tolerance;  // (unused: dlss_actor.frag reads the cameras' buffer's word 66)
};
static_assert(sizeof(Push) == 6 * 8 + 14 * 4 && sizeof(Push) <= 128, "dlss_actor.vert's push constants");

/// A draw's pose in a pose buffer: its origin (12 bytes, at 0), the view it was drawn with (3 x 4 row major, at 16), its
/// projection (4 x 4, at 64), then its bones (at kPoseHead); Push::origins and bones point there
constexpr uint32_t kPoseHead = 128;

/// Push::itype of a draw without indices (a plain vertex array): its vertex i is base_vertex + i
constexpr int kNoIndices = 3;

/// Where a draw's indices are for the shader: the address 4-aligned down, and its first index from there
inline void index_start(uint64_t address, int itype, uint64_t* aligned, uint32_t* first) {
    *aligned = address & ~3ull;
    *first = (uint32_t)((address & 3) >> (itype < 0 ? 0 : itype > 2 ? 2 : itype));
}

/// Push::formats: the position is RGBA16F (else RGB32F); a set's joints are 8-bit (else 16-bit); how many joints and weights
/// a set has (kCountShift + 2 * set: 0 four, 1 one, 2 two: RGBA, R or RG formats); kRigid: no weights, set 0's one joint has
/// all of the vertex (a part that moves with one bone). Joints and weights are read byte by byte, at any offset
constexpr uint32_t kHalfPosition = 1, kByteJoints = 2, kCountShift = 4, kRigid = 1u << 8;
/// The count code of a set of n joints and weights (1, 2 or 4), for Push::formats; 3 (none) for any other n
inline uint32_t count_code(int n) { return n == 4 ? 0u : n == 1 ? 1u : n == 2 ? 2u : 3u; }
/// How many joints and weights a set has, from Push::formats
inline int set_count(uint32_t formats, int set) {
    const uint32_t c = (formats >> (kCountShift + 2 * set)) & 3u;
    return c == 0 ? 4 : c == 1 ? 1 : c == 2 ? 2 : 0;
}

/// The cameras' buffer: this frame's camera (as drawn, jittered), the frame before's (its jitter taken off), this frame's
/// jitter in clip space, the render size (words 0 to 35); then this frame's view (3 x 4, row major) and projection (4 x 4),
/// apart, as the game's vertex shaders apply them (36 to 63); then its near and far planes (0: not a plain perspective
/// projection), for the fragment stage's test in distance (64, 65); the tolerance of that test in thousandths of the
/// distance, kMarkBit for RX_VK_DLSS_DEBUG=actors and kOwnBit with it for =characters (66); the frame before's jitter in clip space (68, 69), which
/// dlss_capture.vert takes off the positions stored then; this frame's clip space to the frame before's (72 to 87, as
/// shaders/dlss_prep.comp's reproject: the camera's motion), which the fragment stage's mark compares the draw's own with
constexpr uint32_t kHeadWords = 88;
constexpr uint32_t kToleranceWord = 66, kJitterBeforeWord = 68, kReprojectWord = 72, kMarkBit = 0x80000000u;
/// (with kMarkBit: the mark is not a motion but kOwnMark in x, which no motion is, and in y how far the draw's own motion
/// is from the camera's, in render pixels: shaders/dlss_prep.comp's flag 8 shows those pixels blue on grey)
constexpr uint32_t kOwnBit = 0x40000000u;
constexpr float kOwnMark = -30000.0f;

/// shaders/dlss_capture.vert's push constants: a draw's slots this frame and the frame before (the address of the first, the
/// vertex index it is, how many)
struct CapturePush {
    uint64_t now;
    uint64_t before;
    uint32_t first_now;
    uint32_t first_before;
    uint32_t count_now;
    uint32_t count_before;
    float jitter_now[2];      // (the jitter of the camera the draw was drawn with, in clip space: this frame's, the frame before's)
    float jitter_before[2];
};
static_assert(sizeof(CapturePush) == 48, "dlss_capture.vert's push constants");

/// The jitter of the camera a draw was drawn with, from its projection (4 x 4 row major, the vertex stage's uniform buffer 3
/// at 0x70): the offset in its third column, as the scene's (vk_temporal.cpp temporal_note_scene_draw). False when it is not
/// a perspective projection (its w row not 0, 0, -1, 0) or the offset is not a sub-pixel one: the scene's is used then
inline bool camera_jitter(const float proj[16], float out[2]) {
    if (proj[12] != 0.0f || proj[13] != 0.0f || proj[14] != -1.0f || proj[15] != 0.0f) return false;
    const float x = -proj[2], y = -proj[6];
    if (!(x > -0.05f && x < 0.05f && y > -0.05f && y < 0.05f)) return false;
    out[0] = x, out[1] = y;
    return true;
}

/// The vertices an indexed draw's count indices name (itype 0, 1, 2: 8, 16, 32 bits): the least and the greatest. With
/// restart, the restart index (all ones) is none. False when there is none
inline bool index_range(const void* indices, int count, int itype, bool restart, uint32_t* lo, uint32_t* hi) {
    if (count <= 0 || itype < 0 || itype > 2) return false;
    const uint32_t size = 1u << itype;
    const uint32_t cut = itype == 0 ? 0xffu : itype == 1 ? 0xffffu : 0xffffffffu;
    uint32_t a = UINT32_MAX, b = 0;
    bool any = false;
    for (int i = 0; i < count; i++) {
        uint32_t v = 0;
        std::memcpy(&v, (const uint8_t*)indices + (size_t)i * size, size);
        if (restart && v == cut) continue;
        a = std::min(a, v), b = std::max(b, v), any = true;
    }
    if (any) *lo = a, *hi = b;
    return any;
}

/// A draw's slots for its stored positions: the vertex index of the first (as the vertex stage sees it, the base vertex in it)
/// and how many, at most max. Indexed (itype 0 to 2): base_vertex + lo to base_vertex + hi; without indices (kNoIndices):
/// base_vertex (the draw's first) and count on. False when there are none, or more than max
inline bool capture_span(int itype, int count, int base_vertex, uint32_t lo, uint32_t hi, uint32_t max, uint32_t* first,
                         uint32_t* n) {
    if (count <= 0) return false;
    uint64_t many;
    if (itype == kNoIndices) *first = (uint32_t)base_vertex, many = (uint64_t)count;
    else if (itype >= 0 && itype <= 2 && lo <= hi) *first = (uint32_t)base_vertex + lo, many = (uint64_t)hi - lo + 1;
    else return false;
    if (many > max) return false;
    *n = (uint32_t)many;
    return true;
}

/// The slot of a vertex (its VertexIndex) in a span, as the vertex stages take it (unsigned, wrapping): false when outside
inline bool capture_slot(int32_t vertex_index, uint32_t first, uint32_t n, uint32_t* slot) {
    const uint32_t s = (uint32_t)vertex_index - first;
    if (s >= n) return false;
    *slot = s;
    return true;
}

/// A clip position stored the frame before, its jitter taken off (dlss_capture.vert; dlss_math::unjitter on a camera)
inline void unjitter_position(float p[4], const float jitter[2]) {
    p[0] -= jitter[0] * p[3];
    p[1] -= jitter[1] * p[3];
}

} // namespace dlss_actor
