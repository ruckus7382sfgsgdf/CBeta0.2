#include "../gpu/vk/dlss_actor.h"

#include <cstdio>

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__); return 1; } } while (0)

int main() {
    namespace da = dlss_actor;
    // names: the same draw keeps its name, another draw of the same model is the next one, another model another name
    const uint64_t a0 = da::key(0x1000, 0x2000, 9702, 0, 0xabc, 0), a1 = da::key(0x1000, 0x2000, 9702, 0, 0xabc, 1);
    CHECK("the same draw, the same name", a0 == da::key(0x1000, 0x2000, 9702, 0, 0xabc, 0));
    CHECK("the second draw of a model is another", a0 != a1 && da::same_model(a0, a1));
    CHECK("another model", !da::same_model(a0, da::key(0x1000, 0x2000, 9702, 3, 0xabc, 0)) &&
                               !da::same_model(a0, da::key(0x1040, 0x2000, 9702, 0, 0xabc, 0)) &&
                               !da::same_model(a0, da::key(0x1000, 0x2000, 9702, 0, 0xabd, 0)));
    CHECK("many draws of one model", da::same_model(a0, da::key(0x1000, 0x2000, 9702, 0, 0xabc, 900)));

    // where a draw's indices start for the shader, which reads 32-bit words: the address 4-aligned down, the first index in it
    uint64_t at = 0;
    uint32_t first = 0;
    da::index_start(0x10002, 1, &at, &first);
    CHECK("16-bit at +2: the second half of the word", at == 0x10000 && first == 1);
    da::index_start(0x10003, 0, &at, &first);
    CHECK("8-bit at +3: the fourth byte", at == 0x10000 && first == 3);
    da::index_start(0x10008, 2, &at, &first);
    CHECK("32-bit, aligned", at == 0x10008 && first == 0);
    da::index_start(0x10004, 1, &at, &first);
    CHECK("16-bit, aligned", at == 0x10004 && first == 0);
    // (a set's joint and weight count in Push::formats: Link's four, five or six bones a vertex, two, and rigid parts)
    const uint32_t five = da::count_code(4) << da::kCountShift | da::count_code(1) << (da::kCountShift + 2);
    CHECK("four, then one", da::set_count(five, 0) == 4 && da::set_count(five, 1) == 1);
    const uint32_t two = da::kByteJoints | da::count_code(2) << da::kCountShift;
    CHECK("two, 8-bit joints", da::set_count(two, 0) == 2 && (two & da::kByteJoints));
    CHECK("the count bits clear of the others",
          !((da::count_code(2) << da::kCountShift | da::count_code(2) << (da::kCountShift + 2)) &
            (da::kHalfPosition | da::kByteJoints | da::kByteJoints << 1 | da::kRigid)));
    CHECK("no other counts", da::count_code(3) == 3 && da::set_count(3u << da::kCountShift, 0) == 0);

    // capture motion: an indexed draw's vertices are its least index to its greatest (the restart index none of them)
    uint32_t lo = 0, hi = 0;
    const uint16_t strip[] = {7, 3, 9, 0xffff, 4, 12};
    CHECK("16-bit, restart on", da::index_range(strip, 6, 1, true, &lo, &hi) && lo == 3 && hi == 12);
    CHECK("16-bit, restart off: all ones is a vertex", da::index_range(strip, 6, 1, false, &lo, &hi) && lo == 3 && hi == 0xffff);
    const uint8_t bytes[] = {200, 5, 255, 17};
    CHECK("8-bit", da::index_range(bytes, 4, 0, true, &lo, &hi) && lo == 5 && hi == 200);
    const uint32_t words[] = {100000, 99999, 0xffffffffu};
    CHECK("32-bit", da::index_range(words, 3, 2, true, &lo, &hi) && lo == 99999 && hi == 100000);
    const uint16_t cuts[] = {0xffff, 0xffff};
    CHECK("only restarts: none", !da::index_range(cuts, 2, 1, true, &lo, &hi) && !da::index_range(strip, 0, 1, true, &lo, &hi));
    CHECK("no such index type", !da::index_range(strip, 6, 3, true, &lo, &hi));
    // its slots: from base vertex + least index, as many as the indices span; a vertex array's from its first, count of them
    uint32_t n = 0;
    CHECK("indexed", da::capture_span(1, 6, 1000, 3, 12, 1u << 20, &first, &n) && first == 1003 && n == 10);
    CHECK("a vertex array", da::capture_span(da::kNoIndices, 36, 250, 0, 0, 1u << 20, &first, &n) && first == 250 && n == 36);
    CHECK("too many", !da::capture_span(2, 3, 0, 0, 1u << 20, 1u << 20, &first, &n));
    CHECK("nothing", !da::capture_span(da::kNoIndices, 0, 0, 0, 0, 1u << 20, &first, &n) &&
                         !da::capture_span(1, 6, 0, 5, 4, 1u << 20, &first, &n));
    CHECK("a negative base vertex", da::capture_span(1, 3, -4, 6, 9, 64, &first, &n) && first == 2 && n == 4);
    // a vertex's slot, as the stages take it: VertexIndex - first, unsigned, below the count
    uint32_t slot = 0;
    CHECK("the first and the last", da::capture_slot(1003, 1003, 10, &slot) && slot == 0 && da::capture_slot(1012, 1003, 10, &slot) &&
                                        slot == 9);
    CHECK("past the last", !da::capture_slot(1013, 1003, 10, &slot));
    CHECK("before the first wraps past the count", !da::capture_slot(1002, 1003, 10, &slot));
    CHECK("index 9 with base vertex -4: the last slot", da::capture_slot(-4 + 9, (uint32_t)(-4 + 6), 4, &slot) && slot == 3);
    // the frame before's position without its jitter: the jitter is the projection's, x + jitter * w
    float p[4] = {0.5f + 0.01f * 2.0f, -0.25f - 0.02f * 2.0f, 0.7f, 2.0f};
    const float jitter[2] = {0.01f, -0.02f};
    da::unjitter_position(p, jitter);
    CHECK("unjittered", p[0] > 0.4999f && p[0] < 0.5001f && p[1] > -0.2501f && p[1] < -0.2499f && p[2] == 0.7f && p[3] == 2.0f);
    CHECK("the push constants", sizeof(da::CapturePush) == 48 && sizeof(da::CapturePush) <= sizeof(da::Push));
    // a draw's own camera's jitter: the projection's third column, negated; none from what is not a perspective projection
    float proj[16] = {1.5f, 0, -0.0004f, 0, 0, 2.6f, 0.0007f, 0, 0, 0, -1.0002f, -0.2f, 0, 0, -1, 0};
    float own[2] = {9, 9};
    CHECK("a jittered projection", da::camera_jitter(proj, own) && own[0] == 0.0004f && own[1] == -0.0007f);
    proj[2] = proj[6] = 0.0f;
    CHECK("one without jitter", da::camera_jitter(proj, own) && own[0] == 0.0f && own[1] == 0.0f);
    proj[14] = 0.0f, proj[15] = 1.0f;
    CHECK("an orthographic one: not taken", !da::camera_jitter(proj, own));
    proj[14] = -1.0f, proj[15] = 0.0f, proj[2] = 0.5f;
    CHECK("an off-centre one: not a jitter", !da::camera_jitter(proj, own));
    CHECK("the cameras' buffer holds its words", da::kJitterBeforeWord + 2 <= da::kHeadWords && da::kToleranceWord < da::kJitterBeforeWord);
    std::puts("dlss_actor: ok");
    return 0;
}
