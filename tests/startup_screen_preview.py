"""Pictures of the start-up screen (gpu/vk/vk_startup_screen.h) with no game, no window and no GPU: builds
tests/startup_screen_preview.cpp (the screen's own drawing code, Dear ImGui, a software rasterizer) and writes a PNG for every
state of the screen, and with --before the screen as it was before the redesign, drawn by the same rasterizer.

    python -B tests/startup_screen_preview.py --out <folder> [--romfs <the game's romfs>] [--size 1920x1080] [--before]
                                              [--only 03-translating] [--large] [--linear] [--no-crest] [--cxx clang++]

--romfs: the player's own game files, read in memory for the game's menu font exactly as the game reads them (nothing of them
is written anywhere but into the pictures); without it the bundled Libertinus is used, as in a game folder that has no font.
Needs Pillow for the PNGs (else the raw .rgba files are left: width, height, RGBA bytes).
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))

IMGUI = ROOT / "third_party" / "imgui"
ZSTD = ROOT / "third_party" / "zstd" / "lib"
ZSTD_SOURCES = ("common/debug.c", "common/entropy_common.c", "common/error_private.c", "common/fse_decompress.c", "common/xxhash.c",
                "common/zstd_common.c", "decompress/huf_decompress.c", "decompress/zstd_ddict.c", "decompress/zstd_decompress.c",
                "decompress/zstd_decompress_block.c")


def header_dir(name, probe, given=""):
    """A package's include folder: the one given, the CPM cache (RX_CPM), HOOKSHOT_VULKAN_HEADERS, or the Vulkan SDK"""
    roots = [given] if given else []
    for cache in (os.environ.get("RX_CPM"), str(ROOT / "cpm")):
        if cache and os.path.isdir(os.path.join(cache, name)):
            roots += [os.path.join(cache, name, d) for d in sorted(os.listdir(os.path.join(cache, name)), reverse=True)]
    for env in ("HOOKSHOT_VULKAN_HEADERS", "VULKAN_SDK"):
        if os.environ.get(env):
            roots.append(os.environ[env])
    for root in roots:
        for inc in ("include", "Include", ""):
            if os.path.isfile(os.path.join(root, inc, *probe.split("/"))):
                return os.path.join(root, inc)
    raise SystemExit("no %s headers (%s): set RX_CPM to a CPM cache" % (name, probe))


def build(cxx, work, headers="", opt="-O2"):
    """The preview program, its objects kept in work (an object is made again when its source is newer). headers: the
    Vulkan headers (gpu/vk/vk_ui.h reaches them through vk_imgui.h)"""
    import logo_rgba
    work.mkdir(parents=True, exist_ok=True)
    logo_rgba.write(str(work / "hookshot_logo.h"))
    win = os.name == "nt"
    target = ["--target=x86_64-pc-windows-msvc", "-fms-extensions", "-D_CRT_SECURE_NO_WARNINGS"] if win and "clang" in cxx else []
    includes = ["-I" + str(ROOT / "gpu" / "vk"), "-I" + str(IMGUI), "-I" + str(ZSTD), "-I" + str(work),
                "-I" + header_dir("vulkanheaders", "vulkan/vulkan.h", headers)]
    if os.name != "nt":   # (the Win32 names the back end's headers use, as its own build has them on POSIX)
        includes += ["-I" + str(ROOT / "host" / "posix"), "-D_GNU_SOURCE"]
    cxxflags = target + ["-std=c++20", opt, "-g0", "-DNDEBUG", "-Wno-everything", "-DVK_NO_PROTOTYPES", "-DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS",
                         "-DIMGUI_DISABLE_WIN32_FUNCTIONS", "-DIMGUI_DISABLE_DEMO_WINDOWS", "-DIMGUI_DISABLE_DEBUG_TOOLS",
                         "-include", "algorithm", "-include", "cstdio", "-include", "cstdlib", "-include", "cstring", "-include", "functional",
                         "-include", "string", "-include", "vector"] + includes
    cflags = target + [opt, "-g0", "-DNDEBUG", "-Wno-everything", "-DZSTD_DISABLE_ASM", "-DZSTD_LEGACY_SUPPORT=0", "-DZSTD_MULTITHREAD=0"]
    cc = cxx.replace("clang++", "clang").replace("g++", "gcc")
    objects = []
    jobs = [(cxx, IMGUI / f, cxxflags, []) for f in ("imgui.cpp", "imgui_draw.cpp", "imgui_tables.cpp", "imgui_widgets.cpp")]
    jobs += [(cc, ZSTD / f, cflags, []) for f in ZSTD_SOURCES]
    ours = [ROOT / "gpu" / "vk" / f for f in ("vk_startup_screen.h", "vk_progress.h", "vk_ui.h", "vk_game_font.h")] + [work / "hookshot_logo.h"]
    jobs.append((cxx, ROOT / "tests" / "startup_screen_preview.cpp", cxxflags, ours))
    for compiler, src, flags, deps in jobs:
        obj = work / (src.name.rsplit(".", 1)[0] + ".o")
        objects.append(str(obj))
        newest = max(os.path.getmtime(p) for p in [src] + deps)
        if obj.exists() and os.path.getmtime(obj) >= newest:
            continue
        subprocess.run([compiler] + flags + ["-c", str(src), "-o", str(obj)], check=True)
    exe = work / ("startup_screen_preview" + (".exe" if win else ""))
    subprocess.run([cxx] + target + objects + ["-o", str(exe)] + ([] if win else ["-lpthread"]), check=True)
    return exe


def to_png(folder):
    try:
        from PIL import Image
    except ImportError:
        print("no Pillow: the pictures stay as .rgba (width, height, RGBA bytes)")
        return
    for raw in sorted(folder.glob("*.rgba")):
        data = raw.read_bytes()
        w, h = struct.unpack_from("<II", data, 0)
        Image.frombytes("RGBA", (w, h), data[8:8 + w * h * 4]).convert("RGB").save(raw.with_suffix(".png"), optimize=True)
        raw.unlink()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--out", required=True, type=Path)
    p.add_argument("--work", type=Path, help="where the program is built (default: <out>/.build)")
    p.add_argument("--romfs", default="-")
    p.add_argument("--size", default="1920x1080")
    p.add_argument("--only", default="")
    p.add_argument("--cxx", default=shutil.which("clang++") or "clang++")
    p.add_argument("--headers", default="", help="the Vulkan headers' folder (default: the CPM cache, RX_CPM)")
    for flag in ("before", "large", "linear", "no-crest"):
        p.add_argument("--" + flag, action="store_true")
    a = p.parse_args()
    exe = build(a.cxx, a.work or a.out / ".build", a.headers)
    a.out.mkdir(parents=True, exist_ok=True)
    cmd = [str(exe), str(a.out), a.romfs, str(ROOT / "third_party" / "fonts" / "libertinus"), "--size", a.size]
    if a.only:
        cmd += ["--only", a.only]
    for flag in ("before", "large", "linear", "no-crest"):
        if getattr(a, flag.replace("-", "_")):
            cmd.append("--" + flag)
    subprocess.run(cmd, check=True)
    to_png(a.out)


if __name__ == "__main__":
    main()
