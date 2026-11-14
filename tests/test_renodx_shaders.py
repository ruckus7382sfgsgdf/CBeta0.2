"""scripts/renodx_shaders.py: its pinned tables, the SHA-256 checks and when the shaders are compiled again, without the
network or the Slang compiler; and gpu/vk/vk_renodx.cpp's two halves (with RenoDX's shaders, and the stubs without).

Run with: python -I tests/test_renodx_shaders.py
"""
import hashlib
import importlib.util
import io
import os
from pathlib import Path
import re
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("renodx_shaders_under_test", ROOT / "scripts" / "renodx_shaders.py")
renodx = importlib.util.module_from_spec(spec)
spec.loader.exec_module(renodx)


def digest(data):
    return hashlib.sha256(data).hexdigest()


class Opener:
    """urlopen over a dict of url -> bytes; records what was asked for"""
    def __init__(self, contents):
        self.contents, self.asked = contents, []

    def __call__(self, url, timeout=None):
        if timeout != renodx.TIMEOUT:
            raise AssertionError("urlopen without the timeout (%r)" % (timeout,))
        self.asked.append(url)
        return io.BytesIO(self.contents[url])


class TableTests(unittest.TestCase):
    def test_pinned(self):
        self.assertRegex(renodx.COMMIT, r"^[0-9a-f]{40}$")
        self.assertIn(renodx.COMMIT, renodx.BASE)
        for path, sha in renodx.FILES:
            self.assertRegex(sha, r"^[0-9a-f]{64}$", path)
            self.assertFalse(path.startswith("/") or ".." in path.split("/"), path)
        for platform, (url, sha, compiler) in renodx.SLANG.items():
            self.assertRegex(sha, r"^[0-9a-f]{64}$", platform)
            self.assertIn(renodx.SLANG_VERSION, url)
            self.assertTrue(url.startswith("https://github.com/shader-slang/slang/releases/download/"), url)
            self.assertTrue(compiler.startswith("bin/slangc"), compiler)

    def test_every_shader_is_fetched(self):
        paths = {p for p, _ in renodx.FILES}
        self.assertIn("LICENSE", paths)
        for _, name, kind, _ in renodx.SHADERS:
            self.assertIn(renodx.GAME + "/" + name, paths)
            self.assertIn(kind, (0, 1, 2))
        self.assertEqual(len({crc for crc, *_ in renodx.SHADERS}), len(renodx.SHADERS))


class FetchTests(unittest.TestCase):
    def test_wrong_file_is_not_kept(self):
        with tempfile.TemporaryDirectory() as tmp:
            dst = os.path.join(tmp, "a", "file")
            with self.assertRaises(ValueError):
                renodx.fetch_one("u", digest(b"expected"), dst, Opener({"u": b"something else"}))
            self.assertFalse(os.path.exists(dst))
            self.assertFalse(os.path.exists(dst + ".part"))

    def test_a_file_already_there_is_not_asked_for(self):
        with tempfile.TemporaryDirectory() as tmp:
            dst = os.path.join(tmp, "file")
            opener = Opener({"u": b"data"})
            self.assertTrue(renodx.fetch_one("u", digest(b"data"), dst, opener))
            self.assertFalse(renodx.fetch_one("u", digest(b"data"), dst, opener))
            self.assertEqual(opener.asked, ["u"])

    def test_a_current_header_downloads_and_compiles_nothing(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertFalse(renodx.current(tmp))
            with open(renodx.header(tmp), "w", encoding="utf-8") as f:
                f.write("// renodx_shaders.py 0000000000000000\n")   # (another script's: made again)
            self.assertFalse(renodx.current(tmp))
            with open(renodx.header(tmp), "w", encoding="utf-8") as f:
                f.write(renodx.stamp() + "\n")
            self.assertTrue(renodx.current(tmp))
            opener = Opener({})
            self.assertEqual(renodx.prepare(tmp, "windows", opener), renodx.header(tmp))
            self.assertEqual(opener.asked, [])


class SourceTests(unittest.TestCase):
    def test_the_renderer_builds_without_the_shaders(self):
        source = (ROOT / "gpu" / "vk" / "vk_renodx.cpp").read_text(encoding="utf-8")
        with_shaders, stubs = source.split("#else", 1)
        self.assertIn('#ifdef HOOKSHOT_RENODX', with_shaders)
        self.assertLess(with_shaders.index("#ifdef HOOKSHOT_RENODX"), with_shaders.index('#include "renodx_spv.h"'))
        header = (ROOT / "gpu" / "vk" / "vk_renodx.h").read_text(encoding="utf-8")
        declared = set(re.findall(r"^(?:[\w:<>*&]+ )+\*?(renodx_\w+)\(", header, re.M))
        self.assertTrue(declared)
        for name in declared:   # (every function of the header is in both halves)
            self.assertIn(name + "(", with_shaders, name)
            self.assertIn(name + "(", stubs, name)
        self.assertIn("bool renodx_built() { return false; }", stubs)

    def test_the_compiled_shaders_are_never_committed(self):
        ignored = (ROOT / ".gitignore").read_text(encoding="utf-8").split("\n")
        self.assertIn(renodx.HEADER, ignored)
        self.assertFalse((ROOT / "gpu" / "vk" / renodx.HEADER).exists(),
                         "gpu/vk/renodx_spv.h is in the tree: the build takes HOOKSHOT_RENODX's, and this one would be included first")


if __name__ == "__main__":
    unittest.main()
