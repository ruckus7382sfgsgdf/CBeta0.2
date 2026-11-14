#version 450
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
// (hookshot) NVIDIA DLSS's actor motion (vk_dlss.cpp): a skinned draw of the scene drawn again, its vertices skinned with this
// frame's bones and with the frame before's, so each pixel it covers gets its own motion and not that of what is behind it.
// The cameras are in a storage buffer (data): this frame's view-projection as the game drew with it, the jitter in it; the frame
// before's with its jitter taken off (both row major, clip = M * position); the jitter; the render size. The vertices and
// indices are read where the game keeps them, the poses (bone palettes of 3 x 4 row-major matrices, origins) where vk_dlss.cpp
// copied them, by their device addresses. The game skins in model space and adds the object's origin:
// world = sum(w * B[j] * p) + origin.
layout(std430, binding = 0) readonly buffer Data { uint w[]; } data;
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer Words { uint w[]; };
layout(push_constant) uniform P {
    uint64_t vertices;   // (the vertex stream's address)
    uint64_t indices;    // (the index buffer's address, 4-aligned down; first: the draw's first index in it)
    uint64_t origins[2]; // (the draw's origin, xyz: this frame's and the frame before's)
    uint64_t bones[2];   // (its palettes: this frame's, the frame before's)
    uint nbones;
    uint stride;    // (in words)
    uint count;     // (vertices in the stream: an index past them draws nothing)
    uint first;
    uint itype;     // (0: 8-bit, 1: 16-bit, 2: 32-bit indices; 3: none, vertex i is base_vertex + i)
    int base_vertex;
    uint pos, sets;          // (byte offsets in a vertex, the joint and weight sets: 1 or 2)
    uint joints[2], weights[2];
    uint formats;            // (dlss_actor.h: 1 the position RGBA16F; 2 << set that set's joints 8-bit; its count at 4 + 2 * set; 256 rigid)
    uint tolerance;          // (the fragment stage's)
} pc;
layout(location = 0) out vec4 before;
layout(location = 1) flat out vec2 jitter;   // (this frame's, of the camera the draw was drawn with: the scene's here)

float f(uint i) { return uintBitsToFloat(data.w[i]); }
vec4 f4(uint i) { return vec4(f(i), f(i + 1), f(i + 2), f(i + 3)); }
mat4 camera(uint at) {   // (row major in the buffer)
    return transpose(mat4(f4(at), f4(at + 4), f4(at + 8), f4(at + 12)));
}
vec4 row(Words m, uint i) { return uintBitsToFloat(uvec4(m.w[i], m.w[i + 1u], m.w[i + 2u], m.w[i + 3u])); }
vec3 skinned(Words palette, uint j, vec4 p) {
    const uint b = j * 12u;
    return vec3(dot(row(palette, b), p), dot(row(palette, b + 4u), p), dot(row(palette, b + 8u), p));
}
uint byte_at(Words v, uint o) { return (v.w[o >> 2] >> ((o & 3u) * 8u)) & 0xffu; }
uint set_count(uint s) {   // (dlss_actor.h set_count)
    const uint c = (pc.formats >> (4u + 2u * s)) & 3u;
    return c == 0u ? 4u : c == 1u ? 1u : c == 2u ? 2u : 0u;
}
void main() {
    jitter = vec2(f(32u), f(33u));
    const uint at = pc.first + uint(gl_VertexIndex);
    uint index;
    if (pc.itype == 3u) index = at;   // (a plain vertex array)
    else if (pc.itype == 2u) index = Words(pc.indices).w[at];
    else if (pc.itype == 1u) index = (Words(pc.indices).w[at >> 1] >> ((at & 1u) * 16u)) & 0xffffu;
    else index = (Words(pc.indices).w[at >> 2] >> ((at & 3u) * 8u)) & 0xffu;
    const int vi = pc.base_vertex + int(index);
    if (vi < 0 || uint(vi) >= pc.count) {   // (outside the stream: nothing, its triangle dropped)
        gl_Position = vec4(0.0, 0.0, 0.0, -1.0);
        before = vec4(0.0);
        return;
    }
    Words vb = Words(pc.vertices + uint64_t(uint(vi)) * uint64_t(pc.stride) * 4ul);
    const uint q = pc.pos / 4u;
    const vec4 p = (pc.formats & 1u) != 0u ? vec4(unpackHalf2x16(vb.w[q]), unpackHalf2x16(vb.w[q + 1u]).x, 1.0)
                                           : vec4(uintBitsToFloat(vb.w[q]), uintBitsToFloat(vb.w[q + 1u]),
                                                  uintBitsToFloat(vb.w[q + 2u]), 1.0);
    vec3 now = vec3(0.0), then = vec3(0.0);
    // (each set: 1, 2 or 4 joints, 8- or 16-bit, and as many 8-bit weights, read byte by byte; rigid: one joint, all of it)
    for (uint s = 0u; s < pc.sets; s++) {
        const bool bytes = (pc.formats & (2u << s)) != 0u;
        const uint n = set_count(s);
        for (uint k = 0u; k < n; k++) {
            const uint jo = pc.joints[s] + (bytes ? k : 2u * k);
            const uint j = bytes ? byte_at(vb, jo) : byte_at(vb, jo) | (byte_at(vb, jo + 1u) << 8);
            const float wt = (pc.formats & 256u) != 0u ? 1.0 : float(byte_at(vb, pc.weights[s] + k)) / 255.0;
            if (wt <= 0.0 || j >= pc.nbones) continue;
            now += wt * skinned(Words(pc.bones[0]), j, p);
            then += wt * skinned(Words(pc.bones[1]), j, p);
        }
    }
    const vec3 o0 = row(Words(pc.origins[0]), 0u).xyz, o1 = row(Words(pc.origins[1]), 0u).xyz;
    // (this frame's position as the game's shaders make it: the origin and the skinned offset through the view apart, never
    // added in the world's large numbers, then the projection. Through the one view-projection, the depth was off by more than
    // the fragment stage's tolerance, and most of a character was left with the camera's motion)
    // (the view and projection the draw itself was drawn with, in its pose: dlss_actor.h kPoseHead)
    Words posed = Words(pc.origins[0]);
    precise vec3 eye;
    for (uint r = 0u; r < 3u; r++) {
        const vec4 v = row(posed, 4u + 4u * r);
        precise float at = dot(v.xyz, o0) + v.w;
        eye[r] = at + dot(v.xyz, now);
    }
    precise vec4 clip;
    for (uint r = 0u; r < 4u; r++) clip[r] = dot(row(posed, 16u + 4u * r), vec4(eye, 1.0));
    before = camera(16u) * vec4(then + o1, 1.0);
    // (the game's clip space: y up, depth -1 to 1; Vulkan's: y down, depth 0 to 1)
    gl_Position = vec4(clip.x, -clip.y, (clip.z + clip.w) * 0.5, clip.w);
}
