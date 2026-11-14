# SPDX-License-Identifier: GPL-3.0-or-later
"""A texture handle read from a table of records in a loop gets one texture per element.

Ocarina of Time's water and wet floors light each pixel with the lights of its screen tile (clustered lighting). A
projected light's shadow map handle is a word of the light's record, read at `table + index * 96 + 96`, with the index
from the tile's light list. No handle program can follow that for a whole draw, and the translation used to fall back
to the first constant word on the handle's path, the light buffer's address: every light then sampled the dummy and
its light stopped at the tile's edges, the squares on the water (#2509).

The texture pass now builds such a handle as a table: the element index becomes `HandleOp::Index` in the handle
program, the texture an array the shader indexes by it, and the draw evaluates the program once per element.

This check compiles the production `HandleOp` enum and `EvaluateHandleProgram` with a synthetic storage buffer and runs
the program the pass builds for the water's record address, in both forms the game's shaders use (a plain multiply,
and an XMAD pair's 16-bit halves of one). It also reads the pass and the draw for the parts that make the table:
the index search, the array's budget counted once, the element passed to the handle, and the bounded storage reads.

python -I -B tests/test_handle_table.py [--cxx c++]
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]


def block(text, start_marker, end_marker):
    start = text.index(start_marker)
    end = text.index(end_marker, start)
    return text[start:end]


def function(text, signature):
    # (the definition: a declaration of the same name, ended by ';' before any '{', is skipped)
    start = text.index(signature)
    while text.index(';', start) < text.index('{', start):
        start = text.index(signature, start + 1)
    brace = text.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def check_sources(root):
    passes = (root / 'gpu/vk/eden/shader_recompiler/ir_opt/texture_pass.cpp').read_text(encoding='utf-8')
    shader = (root / 'gpu/vk/vk_shader.cpp').read_text(encoding='utf-8')
    draw = (root / 'gpu/vk/vk_draw.cpp').read_text(encoding='utf-8')
    info = (root / 'gpu/vk/eden/shader_recompiler/shader_info.h').read_text(encoding='utf-8')

    ops = block(info, 'enum class HandleOp', '};')
    names = [line.strip().split(',')[0].split()[0] for line in ops.splitlines()[1:] if line.strip() and not line.strip().startswith('//')]
    # (programs are kept in hookshot_programs.bin as raw words: an operation added anywhere but at the end renumbers
    # the ones after it)
    assert names[-1] == 'Index', f'HandleOp::Index must be the last operation, not {names[-1]}'

    make = function(passes, 'TextureInst MakeInst(')
    table = make.index('FindHandleTableIndex(')
    legacy = make.index('IR::BreadthFirstSearch(')
    assert table < legacy, 'the table is not tried before the search through loads'
    assert 'RX_VK_HANDLE_TABLE' in make, 'the switch back to the search is gone'
    assert 'g_index_operand = ' in make and 'g_index_operand = nullptr' in make, 'the index is not reset after the build'
    assert 'ShiftLeftLogical(' in make and 'DESCRIPTOR_SIZE_SHIFT' in make, \
        "the shader's array index is not the element index as a descriptor offset"

    node = function(passes, 'bool BuildHandleNode(')
    assert 'inst == g_index_operand' in node and 'Op::Index' in node, 'the index is not built as HandleOp::Index'
    assert 'CompositeExtractU32x4' in node, 'a word of a 128-bit load is not a handle'

    count = function(passes, 'u32 DynamicSampledTextureArrayCount(')
    assert 'handle_program' in count, \
        "a table's samples are counted once each: the water's 69 cut its array to 14 elements"

    of = function(draw, 'template <class D>\nuint32_t handle_of(')
    assert 'stage_handle(s, st, d.handle_program, k)' in of, 'element k does not read record k'

    stage = function(shader, 'uint32_t stage_handle(')
    assert 'element' in stage and 'd.cbuf_offset + 8' in stage, \
        "a table's elements past the storage buffer's bound size are read"
    for kind in ('compute', 'vertex', 'fragment', 'task'):
        assert f'note_fallbacks(p, "{kind}"' in shader, f'the {kind} stage does not log the handles not followed'
    return info


def run_programs(info, cxx):
    enum = block(info, 'enum class HandleOp', '};') + '};'
    evaluate = function(info, 'template <typename ReadCbuf, typename ReadStorage>\nstd::optional<u32> EvaluateHandleProgram(')
    source = r'''
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <span>
#include <vector>
using u32 = std::uint32_t;
using s32 = std::int32_t;
namespace Shader {
''' + enum + '\n' + evaluate + r'''
}
using Shader::HandleOp;
// A light buffer at storage binding 1: 96-byte records after a 96-byte head, word 0 of record i its shadow map
// handle (the water's layout at Zora's River: handles 0x1a902a3c and 0x1a904290 for lights 12 and 13)
static std::vector<u32> lights(4096 / 4);
static u32 cbuf(u32 index, u32 offset) { return index == 0 && offset == 1312 ? 0x3b75a040u : 0; }
static u32 storage(u32 binding, u32 offset) {
    if (binding != 1 || offset + 4 > lights.size() * 4) return 0;
    return lights[offset / 4];
}
static std::vector<u32> op(std::vector<u32> code, HandleOp o) { code.push_back(u32(o)); return code; }
int main() {
    for (u32 i = 0; i < 32; i++) lights[(96 + i * 96) / 4] = 0x1a900000u + 0x100u * i;
    // Plain multiply: c0[1312] + index * 96 + 96 - (c0[1312] & ~15)
    std::vector<u32> plain{u32(HandleOp::Cbuf), 0, 1312, u32(HandleOp::Index), u32(HandleOp::Immediate), 96};
    plain = op(plain, HandleOp::Mul);
    plain = op(plain, HandleOp::Add);
    plain.insert(plain.end(), {u32(HandleOp::Immediate), 96});
    plain = op(plain, HandleOp::Add);
    plain.insert(plain.end(), {u32(HandleOp::Cbuf), 0, 1312, u32(HandleOp::Immediate), 0xfffffff0u});
    plain = op(plain, HandleOp::And);
    plain = op(plain, HandleOp::Sub);
    plain.insert(plain.end(), {u32(HandleOp::Storage), 1});
    // XMAD pair: (index & 0xffff) * 96 + (((index >> 16) & 0xffff) * 96 << 16) + 96
    std::vector<u32> xmad{u32(HandleOp::Index), u32(HandleOp::Immediate), 0xffff};
    xmad = op(xmad, HandleOp::And);
    xmad.insert(xmad.end(), {u32(HandleOp::Immediate), 96});
    xmad = op(xmad, HandleOp::Mul);
    xmad.insert(xmad.end(), {u32(HandleOp::Index), u32(HandleOp::Immediate), 16});
    xmad = op(xmad, HandleOp::Shr);
    xmad.insert(xmad.end(), {u32(HandleOp::Immediate), 0xffff});
    xmad = op(xmad, HandleOp::And);
    xmad.insert(xmad.end(), {u32(HandleOp::Immediate), 96});
    xmad = op(xmad, HandleOp::Mul);
    xmad.insert(xmad.end(), {u32(HandleOp::Immediate), 16});
    xmad = op(xmad, HandleOp::Shl);
    xmad = op(xmad, HandleOp::Add);
    xmad.insert(xmad.end(), {u32(HandleOp::Immediate), 96});
    xmad = op(xmad, HandleOp::Add);
    xmad.insert(xmad.end(), {u32(HandleOp::Storage), 1});
    int failures = 0;
    for (const auto* code : {&plain, &xmad}) {
        for (u32 k = 0; k < 32; k++) {
            const auto h = Shader::EvaluateHandleProgram(std::span<const u32>(*code), cbuf, storage, k);
            const u32 want = 0x1a900000u + 0x100u * k;
            if (!h || *h != want) {
                std::cerr << (code == &plain ? "plain" : "xmad") << " element " << k << ": " << std::hex
                          << (h ? *h : 0xdeadu) << " != " << want << "\n";
                failures++;
            }
        }
    }
    // Without an element, a program evaluates as before: element 0
    const auto zero = Shader::EvaluateHandleProgram(std::span<const u32>(plain), cbuf, storage);
    if (!zero || *zero != 0x1a900000u) { std::cerr << "default element is not 0\n"; failures++; }
    if (failures) return 1;
    std::cout << "ok\n";
    return 0;
}
'''
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, 'handle_table.cpp')
        exe = os.path.join(tmp, 'handle_table.exe' if os.name == 'nt' else 'handle_table')
        with open(src, 'w', encoding='utf-8') as f:
            f.write(source)
        built = subprocess.run([cxx, '-std=c++20', '-O1', src, '-o', exe], capture_output=True, text=True)
        if built.returncode:
            raise SystemExit('compile failed:\n' + built.stderr[-4000:])
        ran = subprocess.run([exe], capture_output=True, text=True)
        if ran.returncode:
            raise SystemExit('handle programs wrong:\n' + ran.stderr[-4000:])


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source-root', type=Path, default=Path(__file__).resolve().parents[1])
    p.add_argument('--cxx', default='c++')
    a = p.parse_args()
    info = check_sources(a.source_root)
    run_programs(info, a.cxx)
    print('PASS: a handle read from a table of records in a loop gets one texture per element')


if __name__ == '__main__':
    main()
