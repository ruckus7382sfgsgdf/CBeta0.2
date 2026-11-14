#version 450
// (hookshot) The temporal upscaler's own motion of a draw drawn again (dlss_actor.vert from its bones, dlss_capture.vert from
// the positions its own vertex stage stored): where the draw is what the scene shows (its distance within the tolerance of
// the scene's), the motion from where it is now (the pixel, the jitter taken off) to where it was, in render pixels as
// shaders/dlss_prep.comp makes them. The tolerance is the cameras' buffer's word 66 (dlss_actor.h kHeadWords), not a push
// constant: the two vertex stages push their own
layout(std430, binding = 0) readonly buffer Data { uint w[]; } data;
layout(binding = 1) uniform sampler2D scene_depth;
layout(location = 0) in vec4 before;
layout(location = 1) flat in vec2 jitter;   // (this frame's, of the camera the draw was drawn with)
layout(location = 0) out vec2 motion;

void main() {
    const uint scene = floatBitsToUint(texelFetch(scene_depth, ivec2(gl_FragCoord.xy), 0).r);
    const uint here = floatBitsToUint(gl_FragCoord.z);
    // (the tolerance's top bit, RX_VK_DLSS_DEBUG=actors: the pixels the draw gives its own motion to marked green, the rest
    // left as they are. Its fragments that are not the scene's are discarded as in play: marked red, the back of a character
    // and what it hides were drawn over the front's green, and most of a character near the camera showed red)
    const uint word = data.w[66];
    const bool mark = (word & 0x80000000u) != 0u;
    const uint tolerance = word & 0x3fffffffu;
    // (in distance from the camera: within tolerance thousandths of the scene's. The game's depth is OpenGL's, -1 to 1 as 0 to 1)
    const float n = uintBitsToFloat(data.w[64]), f = uintBitsToFloat(data.w[65]);
    bool off;
    if (n > 0.0 && f > n) {
        // (n f / (n + (1 - d)(f - n)): the same as 2nf / ((f + n) - (2d - 1)(f - n)), without its cancellation near d = 1 and
        // with a far plane of 1e9 (an infinite one))
        const float zs = n * f / (n + (1.0 - uintBitsToFloat(scene)) * (f - n));
        const float zh = n * f / (n + (1.0 - gl_FragCoord.z) * (f - n));
        off = abs(zh - zs) > zs * float(tolerance) * 0.001;
    } else {
        off = max(scene, here) - min(scene, here) > 64u;   // (no plain perspective: depth buffer steps)
    }
    if (off || before.w <= 1e-6) discard;
    const vec2 size = vec2(uintBitsToFloat(data.w[34]), uintBitsToFloat(data.w[35]));
    const vec2 here_clip = vec2(gl_FragCoord.x / size.x * 2.0 - 1.0, 1.0 - gl_FragCoord.y / size.y * 2.0);
    const vec2 now = here_clip - jitter;
    const vec2 then = before.xy / before.w;
    const vec2 px = vec2(then.x - now.x, now.y - then.y) * 0.5 * size;
    if (!all(lessThan(abs(px), vec2(16384.0)))) discard;
    if (mark) {
        // (only where the draw's own motion is not the camera's, by a quarter of a pixel: with every draw of the scene drawn
        // again from its stored positions, all of it was marked. The camera's as shaders/dlss_prep.comp makes it, from this
        // depth and the scene's jitter. The mark is the one colour: shown as how far apart they are, it could not be told from
        // the camera's motion, which the pixels left unmarked show)
        mat4 reproject;   // (row major in the buffer, as dlss_math::reprojection writes it: read by columns, it was its transpose,
                          // and all that the camera moved was shown as apart)
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++) reproject[c][r] = uintBitsToFloat(data.w[72 + r * 4 + c]);
        const vec4 camera = reproject * vec4(here_clip, gl_FragCoord.z * 2.0 - 1.0, 1.0);
        if (camera.w <= 1e-6) discard;
        const vec2 was = camera.xy / camera.w;
        const vec2 scene_now = here_clip - vec2(uintBitsToFloat(data.w[32]), uintBitsToFloat(data.w[33]));
        const vec2 camera_px = vec2(was.x - scene_now.x, scene_now.y - was.y) * 0.5 * size;
        if (all(lessThan(abs(px - camera_px), vec2(0.25)))) discard;
        // (bit 30 with it, RX_VK_DLSS_DEBUG=characters: no motion but dlss_actor.h kOwnMark, and how far apart the two are,
        // which shaders/dlss_prep.comp's flag 8 shows blue on grey)
        motion = (word & 0x40000000u) != 0u ? vec2(-30000.0, length(px - camera_px)) : vec2(0.0, 64.0);
        return;
    }
    motion = px;
}
