"""The temporal upscaler's own motion (gpu/vk/vk_temporal.cpp, gpu/vk/dlss_actor.h): the CPU side's draw names, index
addresses, index ranges and capture slots (tests/test_dlss_actor.cpp), the push constants of shaders/dlss_actor.vert against
dlss_actor::Push and of shaders/dlss_capture.vert against dlss_actor::CapturePush, dlss_actor.frag reading its tolerance from
the cameras' buffer (the two vertex stages push their own), and the SPIR-V headers of the DLSS shaders made from their sources
as they are now (when glslangValidator is here). No game or GPU.

python -I -B tests/test_dlss_actor.py [--cxx g++]
"""
import argparse
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SHADERS = ROOT / "gpu" / "vk" / "shaders"


def fields(text, start, end):
    """The field names of a block: uint a, b[2]; -> ['a', 'b[2]']"""
    body = text[text.index(start) + len(start):]
    body = body[:body.index(end)]
    body = re.sub(r"//[^\n]*", "", body)
    out = []
    for decl in body.split(";"):
        decl = decl.strip()
        if not decl:
            continue
        names = re.sub(r"^\s*(uint64_t|uint32_t|int32_t|uint|int|float|vec2)\s+", "", decl)
        out += [n.strip() for n in names.split(",")]
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    header = (ROOT / "gpu" / "vk" / "dlss_actor.h").read_text(encoding="utf-8")
    want = fields(header, "struct Push {", "};")
    if len(want) != 16:
        raise SystemExit("dlss_actor::Push: %d fields read: %s" % (len(want), want))
    got = fields((SHADERS / "dlss_actor.vert").read_text(encoding="utf-8"), "uniform P {", "} pc;")
    if got != want:
        raise SystemExit("dlss_actor.vert: its push constants %s are not dlss_actor::Push's %s" % (got, want))
    want_capture = fields(header, "struct CapturePush {", "};")
    capture = (SHADERS / "dlss_capture.vert").read_text(encoding="utf-8")
    got = fields(capture, "uniform P {", "} pc;")
    want_capture = [n.replace("[2]", "") for n in want_capture]   # (float x[2] there is vec2 x here)
    if got != want_capture or len(want_capture) != 8:
        raise SystemExit("dlss_capture.vert: its push constants %s are not dlss_actor::CapturePush's %s" % (got, want_capture))
    # (the cameras' buffer's words, as dlss_actor.h names them)
    words = {n: int(v) for n, v in re.findall(r"(k\w+Word) = (\d+)", header)}
    if words.get("kToleranceWord") != 66 or words.get("kJitterBeforeWord") != 68:
        raise SystemExit("dlss_actor.h: the tolerance and the frame before's jitter are not words 66 and 68: %s" % words)
    frag = (SHADERS / "dlss_actor.frag").read_text(encoding="utf-8")
    if "push_constant" in frag or "data.w[66]" not in frag:
        raise SystemExit("dlss_actor.frag: its tolerance is to be the cameras' buffer's word 66, not a push constant")
    if "pc.jitter_before * then.w" not in capture or "jitter = pc.jitter_now" not in capture:
        raise SystemExit("dlss_capture.vert: the draw's own jitters are to be used (pushed), not the scene's")
    if "flat in vec2 jitter" not in frag:
        raise SystemExit("dlss_actor.frag: this frame's jitter is to come from the vertex stage (the draw's own camera's)")
    chain = (ROOT / "gpu" / "vk" / "vk_temporal.cpp").read_text(encoding="utf-8")
    if "using ActorPush = dlss_actor::Push;" not in chain or "dlss_actor::index_start(" not in chain:
        raise SystemExit("vk_temporal.cpp does not use dlss_actor.h")
    for use in ("dlss_actor::CapturePush", "dlss_actor::index_range(", "dlss_actor::capture_span(", "dlss_actor::kToleranceWord",
                "dlss_actor::kJitterBeforeWord", "dlss_actor::camera_jitter("):
        if use not in chain:
            raise SystemExit("vk_temporal.cpp does not use %s" % use)
    glslang = shutil.which("glslangValidator")
    with tempfile.TemporaryDirectory(prefix="dlss-actor-") as tmp:
        if glslang:
            # (the two passes' -DHDR variants, for an RGBA16F scene: the same sources)
            for src, hdr in (("dlss_actor.vert", False), ("dlss_actor.frag", False), ("dlss_capture.vert", False),
                             ("dlss_prep.comp", False),
                             ("dlss_out.comp", False), ("dlss_merge.comp", False), ("dlss_prep.comp", True),
                             ("dlss_out.comp", True)):
                stem = src.replace(".", "_hdr_" if hdr else "_")
                var = stem.upper() + "_SPV"
                out = Path(tmp) / (stem + "_spv.h")
                define = ["-DHDR"] if hdr else []
                subprocess.run([glslang, "-V", "--target-env", "vulkan1.3"] + define + ["--vn", var, "-o", str(out),
                                str(SHADERS / src)], check=True, capture_output=True)
                words = lambda t: re.findall(r"0x[0-9a-f]{8}", t)
                if words(out.read_text()) != words((SHADERS / out.name).read_text()):
                    raise SystemExit("shaders/%s is not made from %s as it is now (glslangValidator -V --target-env vulkan1.3 "
                                     "%s--vn %s -o %s %s)" % (out.name, src, "-DHDR " if hdr else "", var, out.name, src))
        else:
            print("glslangValidator not found: the SPIR-V headers not checked")
        exe = Path(tmp) / "dlss-actor"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(ROOT / "tests" / "test_dlss_actor.cpp"),
                        "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
