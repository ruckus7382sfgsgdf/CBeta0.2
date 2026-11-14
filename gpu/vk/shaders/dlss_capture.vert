#version 450
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
// (hookshot) The temporal upscaler's motion of what moves by itself (vk_temporal.cpp capture_motion): a draw of the scene drawn
// again from the positions the game's own vertex stage stored as it drew it (the shader recompiler's capture_position:
// clip space as drawn, y up, depth 0 to 1, a vec4 a vertex at its slot), this frame's and the frame before's, so that each
// pixel it covers gets its own motion: bones, waves along normals, morphs, a moving object, whatever the game's shader did.
// It is drawn with the same indices, base vertex and topology as the game drew it, so VertexIndex names the same vertex as it
// did then. The jitters are the draw's own, of the camera it was drawn with each frame (pushed: a draw whose camera has no
// jitter, where the scene's has, was given motion off by the two frames' jitters): the frame before's is taken off here,
// this frame's by the fragment stage.
// dlss_actor.frag tests the depth and writes the motion.
layout(std430, binding = 0) readonly buffer Data { uint w[]; } data;
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Positions { vec4 p[]; };
layout(push_constant) uniform P {
    uint64_t now;        // (this frame's slots: the address of the first)
    uint64_t before;     // (the frame before's)
    uint first_now;      // (the vertex index each one's first slot is)
    uint first_before;
    uint count_now;      // (the slots each one has)
    uint count_before;
    vec2 jitter_now;     // (the draw's camera's jitter in clip space, each frame)
    vec2 jitter_before;
} pc;
layout(location = 0) out vec4 before;
layout(location = 1) flat out vec2 jitter;

void main() {
    jitter = pc.jitter_now;
    const uint i = uint(gl_VertexIndex) - pc.first_now, j = uint(gl_VertexIndex) - pc.first_before;
    if (i >= pc.count_now || j >= pc.count_before) {   // (outside what was stored: nothing, its triangle dropped)
        gl_Position = vec4(0.0, 0.0, 0.0, -1.0);
        before = vec4(0.0);
        return;
    }
    const vec4 now = Positions(pc.now).p[i];
    const vec4 then = Positions(pc.before).p[j];
    // (the jitter is the projection's: clip x + jitter x * w. dlss_math::unjitter's, on the position)
    before = vec4(then.xy - pc.jitter_before * then.w, then.zw);
    // (the game's clip space: y up; Vulkan's: y down. The depth was made 0 to 1 before it was stored)
    gl_Position = vec4(now.x, -now.y, now.z, now.w);
}
