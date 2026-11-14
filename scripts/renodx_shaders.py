"""RenoDX's Zelda OoT shaders for a build with RenoDX's HDR (HOOKSHOT_RENODX, gpu/vk/vk_renodx.cpp): renodx_spv.h.

RenoDX's shaders (src/games/zelda-oot-oz, MIT, Carlos Lopez) are not part of Hookshot and are never committed or bundled, nor is
what is compiled from them: they are worked from the game's own shaders. A build that asks for RenoDX fetches the sources from
RenoDX's repository at one pinned commit and the Slang compiler they are written for at one pinned release, each file checked
against its SHA-256, into <folder>, and compiles the four shaders into <folder>/renodx_spv.h:
  - renodx/: RenoDX's LICENSE and the sources the four shaders read (64 files, 0.7 MB);
  - slang/: the Slang compiler (57 MB, Apache 2.0 with LLVM exception), from its release archive.

RenoDX names the shaders it replaces by the CRC-32 of the SPIR-V another runner's Eden recompiler emits. Hookshot's own emitter
gives the same programs other bytes (its fragment NaN guard, its execution modes), so each replacement is listed here under the
CRC-32 of the module Hookshot makes (RX_VK_DUMP_ALL_SPV=<folder> writes them, named by it). A change to Hookshot's emitter
changes those: dump again and match by what the shader reads.

Usage:  python -I scripts/renodx_shaders.py [--platform linux|windows] [<folder>]
          <folder>: default work/renodx. Then build with HOOKSHOT_RENODX=<folder> (nx2/make_game.py --renodx does both).
        python -I scripts/renodx_shaders.py --from <renodx checkout> <slangc> [<folder>]
          a checkout and a compiler already on the PC: nothing is downloaded
"""
import argparse
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import urllib.request
import zipfile

COMMIT = "59bd6f606a5d105bc6de52627332703c840443be"
BASE = "https://raw.githubusercontent.com/clshortfuse/renodx/" + COMMIT
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME = "src/games/zelda-oot-oz"
HEADER = "renodx_spv.h"

# (Hookshot's module CRC-32, RenoDX's source, its part in the eye adaptation (0 none, 1 the fog: it records the field of view,
# 2 the scene: it meters), what it is)
# (Triforce, 11-13: matched again for this translator, on an NVIDIA card. The patch's four (C749AC23, D2EDEE38, 0D00542E,
# 9E0C0C3A) were v0.3.3's, and none of them is made any more: RenoDX replaced nothing. Found by interface: of the 6,503
# fragment modules a run in HDR Native made (RX_VK_DUMP_ALL_SPV), those with the same set 0 bindings, inputs and outputs as
# RenoDX's shader: one each for the fog and the local exposure's scene, 8 for the encoder and 6 for the scene, of which one
# each is drawn in play (RX_VK_RENODX_TRACE=1: the frame's last draw, and the draw after the bloom's pyramid). Another card's
# make can give these programs other bytes (the AMD and Intel translation fixes): there RenoDX replaces nothing until its
# four are matched the same way, and the log says so half a minute in ("none of its four shaders has replaced"))
SHADERS = [
    (0x4AE77AB6, "output_0x92D6F81D.frag.slang", 0, "the native HDR output encoder"),
    (0x66644A1B, "scene_mapping_hdr_0xED732BB7.frag.slang", 2, "the scene's tone mapping, native HDR (what play draws)"),
    (0xD08E7AD8, "scene_mapping_hdr_0x09EB4538.frag.slang", 2, "the scene's tone mapping, native HDR, with local exposure"),
    (0xCCB24C45, "fov_capture_0x0258153C.frag.slang", 1, "the fog, which records the camera's field of view"),
]
FLAGS = ["-stage", "fragment", "-O3", "-g0", "-entry", "main", "-Wno-30056", "-Wno-15205", "-Wno-30081", "-Wno-41035",
         "-DNDEBUG", "-target", "spirv"]

# (path in RenoDX's repository, its SHA-256): its license, and what the four shaders read (slangc -depfile lists them)
FILES = [
    ("LICENSE", "e9e2da3991a48d9c09b710c9ddbf5673275b6d569c976bcffab3111d0c509d51"),
    ("src/games/zelda-oot-oz/aa_controls.slangi", "aece31045cedc6b25a7995052c4fa6915afb23b3e9f47c7703f55562aa34c646"),
    ("src/games/zelda-oot-oz/adaptation.slangi", "6964a8809c86fb426de776a30fa39f0f960311b1628d80304e11da6d674e6f27"),
    ("src/games/zelda-oot-oz/fov_capture_0x0258153C.frag.slang", "05eda5c10f28d7d7bc469766f9488be313ad5e9ddba81f926c1a009c7555c76c"),
    ("src/games/zelda-oot-oz/output_0x92D6F81D.frag.slang", "8fa2f3b6e10b61212e1f29891487522ecd617cead3ab3dc3e463ed163c1dd15d"),
    ("src/games/zelda-oot-oz/scene_mapping_hdr_0x09EB4538.frag.slang", "f4b7a1aef605aa3842e804110d7ade1b2362505195a94673a90bb3c6c50a8856"),
    ("src/games/zelda-oot-oz/scene_mapping_hdr_0xED732BB7.frag.slang", "bb1e12b20fed3cf5d5a2d23952557bcf1ab55f4833e1774975a2373e0c6f15ce"),
    ("src/games/zelda-oot-oz/scene_tonemap.slangi", "41d94e7c8cab28c7c966fda36f43167fb5eeb411b2265cd717631d22642dbf7c"),
    ("src/games/zelda-oot-oz/shared.h", "9e3d9748d37dc98f244af56ae3005f61535cca2ccd67d5c42d3f7a852e4197a3"),
    ("src/shaders/canvas.hlsl", "7c721ee6ac76177223194345d1229fa8a09a36f0d92f437960ccfbeeef568be5"),
    ("src/shaders/color.hlsl", "bf277992a11f1c3ab3f74d86fd67f47326ef10f57b868bd701f5446de7033ae1"),
    ("src/shaders/color/acescc.hlsl", "293fd2c95666c73938e6a69cfd61020f6d4dfc55f1269fc92263a179e0d70ea8"),
    ("src/shaders/color/acescct.hlsl", "1502d905d75eee1a548ca594ded3e8a261d8227c469639c4aac06c7cf515e74f"),
    ("src/shaders/color/arri.hlsl", "07c0383c7d9289d98dde2431f03d8d50747ba9985f5f0b865df6db968e23013c"),
    ("src/shaders/color/castlecsf.hlsl", "4a4373ecc66b933ef0b3f5d9c92b1b00f983a6ffffb90ed8a72567403dfdd90f"),
    ("src/shaders/color/clamp.hlsl", "1c1165062f6a2a1d37db59285fd39ceff041d85e6b710ea41628e7af06b6dcc0"),
    ("src/shaders/color/colorvideovdp.hlsl", "1784d60bb2d89925f9b1c5ace038c1423b196f49d1f2c40a2cc7b825a4ee4873"),
    ("src/shaders/color/convert.hlsl", "79f60e7501b6454a7f78e88442b5f74c9a17b676ff2f0f44fae13edd56b642c9"),
    ("src/shaders/color/dtucs.hlsl", "ea4e914c78d73200ace2a8295e7c0afadb236b47da4492742e7f2cdb1f1e2d98"),
    ("src/shaders/color/gamma.hlsl", "1c24fb8a3032fbd730f44fa5d2f7d8cdd5857646c4f9c9ceedbd055879544122"),
    ("src/shaders/color/gamut.hlsl", "4030044bf73e6e46cc927248f7e5b76e3f3d92336318b72adb5ed2875bc204ae"),
    ("src/shaders/color/ictcp.hlsl", "7c88d88a1eddc0985cb391c396cc68dedb1c581958bc65b0637e6d40fa2fe759"),
    ("src/shaders/color/ipt.hlsl", "598874398019c930c847c0ada9d2825c5ea2621d104f48b9a1b2c54e375bd53a"),
    ("src/shaders/color/macleod_boynton.hlsl", "c1b89facaa91ff6662746b0a12f1403f961ce116ee94279604a27e35a61c49e0"),
    ("src/shaders/color/oklab.hlsl", "a97b6115f7aa728c5bbf45c25361ae1a15d5777c21b393c30adea73502d2b787"),
    ("src/shaders/color/pq.hlsl", "06b13cb8b9c2fa37817c20ca41977e005cd3ee53ea0c5fd54f3fd44824b934b9"),
    ("src/shaders/color/rgb.hlsl", "babbb614ede608d8250f15b1444e4e617a46fe7d048d7711f7a406f51e29d83c"),
    ("src/shaders/color/srgb.hlsl", "8ea6d0b188a2802f137bc9b1a9fdb00913323da3225f313d96ec8ef2f32e1a7e"),
    ("src/shaders/color/stockman.hlsl", "bcaeba9be68abbb66dc11b0f6018c76e7ede2832fe9ee96364a49ec02c953011"),
    ("src/shaders/color/ycbcr.hlsl", "3e0f395276b91c79be0feb8723d20e200cab7418dda00c2d8d9de96c7130054c"),
    ("src/shaders/color_convert.hlsl", "6d1655efe39f80f3bb96eef6dba4a03565678575e7bf7aaabe748cb876982dc4"),
    ("src/shaders/colorcorrect.hlsl", "18c63e8aeede808a83c60c8d03fb96eef05e68c8a966af6c8e08d6ee67c3528d"),
    ("src/shaders/colorgrade.hlsl", "d2acff2015f912ee981f511482d6199e849c96db4a18bfc888659c1f32c8f98f"),
    ("src/shaders/debug.hlsl", "4b364d342c4bf1b1b858ce473f5aa9b880e5b2908432e63cb3508172f9f17e7b"),
    ("src/shaders/deprecated.hlsl", "5a974b3e481f38ff2278fee2afa248eb07163066e3907ebc4bbb9d003b4eafe1"),
    ("src/shaders/draw.hlsl", "0f873ee298929b48999c5bb36037dcc8e13bd4b76e38124cc0ebba38c752aac9"),
    ("src/shaders/effects.hlsl", "412d95b9248b9a9b7e374cf8acad994cea687510788cbc9a69ce46d7d26ca9bf"),
    ("src/shaders/inverse_tonemap.hlsl", "69653d5cde82f3dbd0d97645afeebaec01c1ba42149435521134861910c99151"),
    ("src/shaders/lut.hlsl", "d7ed65ebcc1979a6c3b490a0ed3acf608d4daa8d2fc4bbd696cd9f6336f71dc8"),
    ("src/shaders/math.hlsl", "fb4c282c2baf8bf75c5de895c1a6fb7f5798b978cb537cc5032a7a764680e673"),
    ("src/shaders/math/constants.hlsl", "96ff24c68515fcda1e95c203f7b9be06ec8057179bb145a4d0f03c7619820fe2"),
    ("src/shaders/math/cross.hlsl", "b81755c7964cb8447283d2ac33376333062b4d39d635296b4272f5198fd8e08d"),
    ("src/shaders/math/select.hlsl", "c1f28149a9708fa06ce137c7634221a7a2ce3f6ebcc46a1f386408eea2a3d6c2"),
    ("src/shaders/math/sign.hlsl", "54841f0f2efe9a818ca32d2462e03139a7fcc86a398350559271e15523d60e92"),
    ("src/shaders/random.hlsl", "a1d560bfffe016c00661f29b749101b94350ea3cde911b46063e1d9f0ac16f0f"),
    ("src/shaders/renodx.hlsl", "4e8a033c989f79fc64b175e7ef8b122c8a5c57609e5a1f56786b1bbb32108bcf"),
    ("src/shaders/tonemap.hlsl", "221c3588c855cbf636abecda4c66d6824e32a260505cb7490dcac1847cbcf94f"),
    ("src/shaders/tonemap/aces.hlsl", "54c64cbeab4b6fabdb916d4eb92768258facb5de420a32112e2d336bd90cc5bc"),
    ("src/shaders/tonemap/agx.hlsl", "0b58d91deed404d1b0af094e61bc663dc6c8c0979b20c9d7a8d4e00db42c1a23"),
    ("src/shaders/tonemap/allenwp.hlsl", "55572ee3b24d20a70793cf8b9461a2a06656e2e8e6a13747880e57735406c534"),
    ("src/shaders/tonemap/c_infinity_rolloff.hlsl", "dd16c280bc7a0a773d000bbd40e9d9e154ed7de4d39c7caeb6c376b28ffa5d45"),
    ("src/shaders/tonemap/daniele.hlsl", "3919f5675668e6c5e0eb3843b09b1a20ce530904fc31014a358bbbc8fd7fc52d"),
    ("src/shaders/tonemap/DICE.hlsl", "21dc120417df088226ced464d4921136a4134d1bfa24104a2569f40a9a07c11e"),
    ("src/shaders/tonemap/frostbite.hlsl", "a962696ec9246376cf4811eb2ca72f3a7997fc0c63b630886f4551618194844b"),
    ("src/shaders/tonemap/hermite_spline.hlsl", "b70a83c88f4a9ec3dd669fb3ef574cb94010e17804ed403dcd6d3e94a0af2cb5"),
    ("src/shaders/tonemap/naka_rushton.hlsl", "e3151899bf3f7bb7407176c93c00dbddf2a0437c4056156d4fe2492d79e1ed16"),
    ("src/shaders/tonemap/neutwo.hlsl", "49982526e49be628f94290f021e96bd404b1ddee54475984a3807cc54ff118bd"),
    ("src/shaders/tonemap/psychov.hlsl", "521b4a6772da427369f4a34122cf91357758ad46a2ff535d9b27793547ecf574"),
    ("src/shaders/tonemap/psychov/test17.hlsl", "363aae484a157e2cf5c8feea23373315058891f51ae731df7ba72267417aec89"),
    ("src/shaders/tonemap/psychov/test22.hlsl", "52b9468ace1973dfbdf2afbeb2f4e7b3294237ab8411d903ab22a7a86310eb07"),
    ("src/shaders/tonemap/psychov/test30.hlsl", "edf8366a568771f0e66f5ead24b8aca93f5f0b7f93b922621e0900eb7dcce185"),
    ("src/shaders/tonemap/psychov/test35.hlsl", "a420abc34dc690cc761d97803a9697a862c5c3a43712bac3dde029e8342d1216"),
    ("src/shaders/tonemap/reinhard.hlsl", "7b4e78632482895e43544969c19bdd412165f57c5edf3049b2bd8f2edd169eac"),
    ("src/shaders/tonemap/reno_drt.hlsl", "54c940b810e0e7c2bdc62a6a3071d1c57c9de5403b0a260cc9a7defc17ff4dba"),
    ("src/shaders/tonemap/rushton_henry.hlsl", "b8fae1e57684c2060ce65a4184af6f6f303a12a3908448fcee42fa7c6dd6df2d"),
]

# (the Slang compiler's release archive for a platform: its URL, its SHA-256, the compiler in it)
SLANG_VERSION = "2026.14.1"
SLANG_BASE = "https://github.com/shader-slang/slang/releases/download/v%s/slang-%s-" % (SLANG_VERSION, SLANG_VERSION)
SLANG = {
    "windows": (SLANG_BASE + "windows-x86_64.zip", "5ed0a59d650a0af0aca45d5db4e083b3d8fb5cea05748747dd95dfbe9c580658",
                "bin/slangc.exe"),
    "linux": (SLANG_BASE + "linux-x86_64.zip", "d7c02e044b5f2f4ceb8e220a2cf8f7d053418db1877da2656fb59e1ce545f8dd",
              "bin/slangc"),
}

NOTICE = """\
RenoDX's HDR is built from RenoDX's Zelda OoT shaders (github.com/clshortfuse/renodx, MIT, Carlos Lopez), compiled on this PC
with the Slang compiler (github.com/shader-slang/slang). Neither they nor what is compiled from them is part of Hookshot:
never share a game folder or a build made with them."""


def host_platform():
    return "linux" if sys.platform.startswith("linux") else "windows"


def header(folder):
    """The compiled shaders in a HOOKSHOT_RENODX folder: what nx2/build.py's renderer includes"""
    return os.path.join(folder, HEADER)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


TIMEOUT = 60   # (seconds without an answer, or between two reads, before a download gives up: a stalled one ends the build)


def fetch_one(url, digest, dst, opener=urllib.request.urlopen):
    """One file into dst, unless it is there already with that SHA-256. The download goes to dst.part and is moved in only
    when its SHA-256 is the expected one; otherwise it is deleted and ValueError says which"""
    if os.path.isfile(dst) and sha256(dst) == digest:
        return False
    os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
    tmp = dst + ".part"
    try:
        with opener(url, timeout=TIMEOUT) as r, open(tmp, "wb") as f:
            shutil.copyfileobj(r, f, 1 << 20)
        if sha256(tmp) != digest:
            raise ValueError("renodx: %s is not the file expected (SHA-256): not used" % url)
        os.replace(tmp, dst)
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)
    return True


def slangc(folder, platform):
    return os.path.join(folder, "slang", *SLANG[platform][2].split("/"))


def fetch(folder, platform, opener=urllib.request.urlopen, say=print):
    """RenoDX's sources and the Slang compiler into the folder: only what is missing or not the file expected is downloaded.
    The compiler's archive is unpacked again when it is another one than the folder's (slang/archive.sha256 says which)"""
    for path, digest in FILES:
        if fetch_one(BASE + "/" + path, digest, os.path.join(folder, "renodx", *path.split("/")), opener):
            say("renodx: %s" % path)
    url, digest, _ = SLANG[platform]
    slang = os.path.join(folder, "slang")
    mark = os.path.join(slang, "archive.sha256")
    if os.path.isfile(slangc(folder, platform)) and os.path.isfile(mark) and open(mark).read().strip() == digest:
        return
    archive = os.path.join(folder, "slang.zip")
    say("renodx: the Slang compiler %s (57 MB on Windows, 80 MB on Linux)" % SLANG_VERSION)
    fetch_one(url, digest, archive, opener)
    shutil.rmtree(slang, ignore_errors=True)
    with zipfile.ZipFile(archive) as z:
        root = os.path.realpath(slang)
        # (the compiler and its libraries and licenses alone: the archive's documentation has paths past Windows' 260
        # characters in a deep folder. Nothing outside the folder, whatever the archive names)
        wanted = [i for i in z.infolist() if not i.is_dir() and i.filename.split("/")[0] in ("bin", "lib", "LICENSE", "LICENSES")]
        for info in wanted:
            if not os.path.realpath(os.path.join(slang, info.filename)).startswith(root + os.sep):
                raise ValueError("renodx: the Slang archive names a file outside its folder: not used")
        z.extractall(slang, wanted)
    if platform == "linux":   # (a zip keeps no modes)
        for name in os.listdir(os.path.dirname(slangc(folder, platform))):
            os.chmod(os.path.join(os.path.dirname(slangc(folder, platform)), name), 0o755)
    with open(mark, "w") as f:
        f.write(digest + "\n")
    os.remove(archive)


def stamp():
    """What a compiled header is made from: the commit, the compiler, the shaders and their flags. In the header's first line,
    so one made from something else is made again"""
    h = hashlib.sha256(repr((COMMIT, SLANG_VERSION, SHADERS, FLAGS, FILES)).encode())
    return "// renodx_shaders.py %s" % h.hexdigest()[:16]


def current(folder):
    """The folder's header is this script's: compiled from the pinned sources with the pinned compiler"""
    path = header(folder)
    if not os.path.isfile(path):
        return False
    with open(path, encoding="utf-8") as f:
        return f.readline().rstrip("\n") == stamp()


def compile_header(renodx, compiler, out_path, say=print, first_line=None):
    """renodx_spv.h from a RenoDX checkout (or the folder's renodx/) with slangc"""
    src = os.path.join(renodx, *GAME.split("/"))
    lines = ([first_line] if first_line else []) + [
        "// Generated by scripts/renodx_shaders.py from RenoDX (MIT, Carlos Lopez): do not edit, never commit.",
        "// Each array replaces the module of Hookshot's whose CRC-32 names it (vk_renodx.cpp).", "#pragma once",
        "#include <cstdint>", ""]
    with tempfile.TemporaryDirectory() as tmp:
        for crc, name, kind, what in SHADERS:
            spv = os.path.join(tmp, "%08X.spv" % crc)
            r = subprocess.run([compiler, name] + FLAGS + ["-o", spv], cwd=src, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            if r.returncode or not os.path.isfile(spv):
                raise ValueError("renodx: %s did not compile (%s)" % (name, r.stdout.decode("utf-8", "replace").strip()[-400:]))
            data = open(spv, "rb").read()
            words = struct.unpack("<%dI" % (len(data) // 4), data)
            lines.append("// %s: %s" % (name, what))
            lines.append("static const uint32_t RENODX_%08X[] = {" % crc)
            for i in range(0, len(words), 12):
                lines.append("    " + ", ".join("0x%08x" % w for w in words[i:i + 12]) + ",")
            lines.append("};")
            say("renodx: %08X <- %s (%d words)" % (crc, name, len(words)))
    lines.append("")
    lines.append("static const struct { uint32_t crc; const uint32_t* words; uint32_t count; int kind; } RENODX_SHADERS[] = {")
    for crc, name, kind, what in SHADERS:
        lines.append("    {0x%08Xu, RENODX_%08X, sizeof RENODX_%08X / 4, %d}," % (crc, crc, crc, kind))
    lines.append("};")
    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(out_path + ".part", "w", newline="\n", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    os.replace(out_path + ".part", out_path)


def prepare(folder, platform=None, opener=urllib.request.urlopen, say=print):
    """The folder ready for a build with HOOKSHOT_RENODX=<folder>: fetched, and its header compiled when it is not this
    script's already. Returns the header"""
    platform = platform or host_platform()
    if current(folder):
        return header(folder)
    fetch(folder, platform, opener, say)
    compile_header(os.path.join(folder, "renodx"), slangc(folder, platform), header(folder), say, stamp())
    return header(folder)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--platform", choices=sorted(SLANG), default=host_platform())
    p.add_argument("--from", dest="local", nargs=2, metavar=("CHECKOUT", "SLANGC"),
                   help="a RenoDX checkout and a slangc already on the PC: nothing is downloaded")
    p.add_argument("folder", nargs="?", default=os.path.join(ROOT, "work", "renodx"))
    a = p.parse_args(argv)
    print(NOTICE, flush=True)
    try:
        if a.local:
            compile_header(a.local[0], a.local[1], header(a.folder))
        else:
            prepare(a.folder, a.platform)
    except (OSError, ValueError) as e:
        print(e, file=sys.stderr)
        return 1
    print("renodx: %s (build with HOOKSHOT_RENODX=%s)" % (header(a.folder), a.folder))
    return 0


if __name__ == "__main__":
    sys.exit(main())
