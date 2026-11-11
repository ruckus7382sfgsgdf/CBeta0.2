// Runs the shader lowering over every stage in triforce_shaders.bin (HKSEED01: every shader the game ships), exactly as
// vk_shader.cpp's parse_code does: the new lowering first, the old REGMAP one when the new one rejects a stage. Prints each
// stage that is not a plain "new ok", then totals. A stage neither lowering takes is a program that is never drawn.
//   clang++ -std=c++20 -O2 -w -I gpu/vk/eden -I gpu/vk/compat tests/seed_lowering.cpp -o seed_lowering.exe
//   seed_lowering <game folder>/triforce_shaders.bin [mma] [<file: one line per lowered stage, for a before/after diff>]
// 11-10: 26 stages failed (36 programs); 3 after the register-pressure forms, USHF.R.U64 and the guarded 64-bit uniform adds.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell_regmap.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"

static uint64_t hash_bytes(const void* p, size_t n, uint64_t h = 0xcbf29ce484222325ull) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}
constexpr uint32_t kControlMagic = 0x98761234, kControlSize = 0xd8, kAmpereHeader = 128;

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const bool mma = argc > 2 && !strcmp(argv[2], "mma");
    FILE* f = fopen(argv[1], "rb");
    if (!f) return 3;
    _fseeki64(f, 0, SEEK_END);
    std::vector<uint8_t> blob((size_t)_ftelli64(f));
    _fseeki64(f, 0, SEEK_SET);
    if (fread(blob.data(), 1, blob.size(), f) != blob.size() || memcmp(blob.data(), "HKSEED01", 8)) return 4;
    fclose(f);
    const uint32_t nprograms = *(uint32_t*)(blob.data() + 8), nstages = *(uint32_t*)(blob.data() + 12);
    size_t at = 16;
    unsigned long long all_hash = 1469598103934665603ull;
    FILE* list_ok = argc > 3 ? fopen(argv[3], "w") : nullptr;   // (each lowered stage's own hash, for a before/after diff)
    int n_ok = 0, n_old = 0, n_fail = 0, n_hdr = 0, n_bad = 0, n_invalid = 0;
    std::map<std::string, int> why_old, why_fail;
    int by_stage_total[8]{}, by_stage_old[8]{}, by_stage_fail[8]{};
    std::vector<int> outcome(nstages, 0);   // 0 ok, 1 old, 2 fail, 3 not a program
    std::vector<uint64_t> keys(nstages, 0);
    std::vector<uint32_t> stage_of(nstages, 0);
    for (uint32_t s = 0; s < nstages; s++) {
        const uint8_t* h = blob.data() + at;
        const uint32_t csize = *(uint32_t*)(h + 12), bytes = *(uint32_t*)(h + 16);
        const size_t control = at + 20, code = control + csize;
        at = code + bytes;
        if (at > blob.size()) { fprintf(stderr, "truncated\n"); return 5; }
        const uint8_t* ctl = blob.data() + control;
        uint32_t magic, len, c1o, end;
        uint16_t st;
        memcpy(&magic, ctl, 4), memcpy(&len, ctl + 0x38, 4), memcpy(&c1o, ctl + 0x40, 4), memcpy(&end, ctl + 0x44, 4), memcpy(&st, ctl + 0x56, 2);
        if (csize < kControlSize || magic != kControlMagic || !len || end > (16u << 20) || c1o > end || len > end || st > 7 || end > bytes) {
            n_bad++, outcome[s] = 3;
            continue;
        }
        const uint8_t* area = blob.data() + code;
        const uint64_t key = hash_bytes(area, end, hash_bytes(ctl, kControlSize));
        keys[s] = key, stage_of[s] = st;
        by_stage_total[st]++;
        const bool graphics = st != 5;
        const uint32_t hdr = graphics ? kAmpereHeader : 0;
        if (graphics && len <= hdr + 16) { n_hdr++; continue; }
        using Shader::Ampere::Instruction;
        std::vector<Instruction> native;
        Shader::Ampere::ProgramCaptureState capture;
        bool invalid = false;
        for (uint32_t i = 0; hdr + (i + 1) * sizeof(Instruction) <= len; i++) {
            Instruction& insn = native.emplace_back();
            memcpy(&insn, area + hdr + i * sizeof(Instruction), sizeof insn);
            const auto r = capture.Observe(insn, i);
            if (r == Shader::Ampere::ProgramCaptureState::Result::InvalidTarget) { invalid = true; break; }
            if (r == Shader::Ampere::ProgramCaptureState::Result::Complete) break;
        }
        if (invalid) {
            n_invalid++, n_fail++, outcome[s] = 2, by_stage_fail[st]++;
            why_fail["capture: invalid branch target"]++;
            printf("%u %016llx FAIL capture: invalid branch target\n", st, (unsigned long long)key);
            continue;
        }
        Shader::Ampere::native_matrix_multiply = Shader::AmpereRegmap::native_matrix_multiply = mma && !graphics;
        const auto lowered = Shader::Ampere::LowerToMaxwell(native);
        if (lowered) {
            n_ok++;
            unsigned long long h = 1469598103934665603ull;
            for (auto w : lowered.instructions) h = (h ^ w) * 1099511628211ull;
            all_hash = (all_hash ^ h ^ key) * 1099511628211ull;
            if (list_ok) fprintf(list_ok, "%u %016llx %zu %016llx\n", st, (unsigned long long)key, lowered.instructions.size(), h);
            continue;
        }
        const uint32_t ia = lowered.error_offset / 16;
        const unsigned op = ia < native.size() ? (unsigned)native[ia].Bits(0, 12) : 0xffffffffu;
        char why[200];
        snprintf(why, sizeof why, "%s (op %#x)", std::string(lowered.error).c_str(), op);
        char words[96] = "";
        if (ia < native.size())
            snprintf(words, sizeof words, " @%u/%zu %08x %08x %08x %08x", ia, native.size(), native[ia].words[0], native[ia].words[1],
                     native[ia].words[2], native[ia].words[3]);
        const auto old = Shader::AmpereRegmap::LowerToMaxwell(
            std::span{reinterpret_cast<const Shader::AmpereRegmap::Instruction*>(native.data()), native.size()});
        if (old) {
            n_old++, outcome[s] = 1, by_stage_old[st]++;
            why_old[why]++;
            printf("%u %016llx OLD %s%s%s\n", st, (unsigned long long)key, why, old.register_map_words ? " [old: extended registers]" : "", words);
        } else {
            const uint32_t oa = old.error_offset / 16;
            const unsigned oop = oa < native.size() ? (unsigned)native[oa].Bits(0, 12) : 0xffffffffu;
            n_fail++, outcome[s] = 2, by_stage_fail[st]++;
            why_fail[why]++;
            printf("%u %016llx FAIL %s [old: %s (op %#x)]%s\n", st, (unsigned long long)key, why, std::string(old.error).c_str(), oop, words);
        }
    }
    // programs (sets of stages): how many have a stage that fails, or one that takes the old lowering
    int p_fail = 0, p_old = 0;
    for (uint32_t p = 0; p < nprograms; p++) {
        const uint32_t n = *(uint32_t*)(blob.data() + at);
        const uint32_t* list = (const uint32_t*)(blob.data() + at + 4);
        at += 4 + (size_t)n * 4;
        bool fl = false, ol = false;
        for (uint32_t k = 0; k < n; k++)
            if (list[k] < nstages) fl |= outcome[list[k]] == 2, ol |= outcome[list[k]] == 1;
        p_fail += fl, p_old += !fl && ol;
    }
    printf("\n== %u stages, %u programs (native matrix multiplies %s)\n", nstages, nprograms, mma ? "on" : "off");
    printf("== stages: %d lowered by the new lowering, %d only by the old one, %d by neither; %d header-only, %d not programs (%d bad branch targets)\n",
           n_ok, n_old, n_fail, n_hdr, n_bad, n_invalid);
    if (list_ok) fclose(list_ok);
    printf("== hash of everything the new lowering made: %016llx\n", all_hash);
    printf("== programs: %d have a stage that does not lower at all (never drawn), %d more use the old lowering for a stage\n", p_fail, p_old);
    printf("== by NVN stage (0 vertex, 1 fragment, 5 compute, 6 task, 7 mesh): total / old / fail\n");
    for (int k = 0; k < 8; k++)
        if (by_stage_total[k]) printf("   stage %d: %d / %d / %d\n", k, by_stage_total[k], by_stage_old[k], by_stage_fail[k]);
    printf("== neither lowering takes it, by the new lowering's reason:\n");
    for (const auto& [w, n] : why_fail) printf("   %4d  %s\n", n, w.c_str());
    printf("== only the old lowering takes it, by the new lowering's reason:\n");
    for (const auto& [w, n] : why_old) printf("   %4d  %s\n", n, w.c_str());
    return 0;
}
