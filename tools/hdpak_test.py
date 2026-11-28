"""Tests of tools/hdpak.py (python3 tools/hdpak_test.py; needs Pillow and numpy)."""

import io
import os
import sys
import tempfile
import unittest

import numpy as np
from PIL import Image, PngImagePlugin

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hdpak  # noqa: E402


def png(rgba: np.ndarray, texts: dict[str, str] | None = None, mode: str = "RGBA") -> bytes:
    info = PngImagePlugin.PngInfo()
    for k, v in (texts or {}).items():
        info.add_text(k, v)
    im = Image.fromarray(rgba, "RGBA")
    if mode == "P":
        im = im.convert("P", palette=Image.Palette.ADAPTIVE, colors=16)
    out = io.BytesIO()
    im.save(out, "PNG", pnginfo=info)
    return out.getvalue()


def pixels(data: bytes) -> np.ndarray:
    return np.asarray(Image.open(io.BytesIO(data)).convert("RGBA"))


class HdpakTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = self.tmp.name
        self.src = os.path.join(self.root, "pack")
        rng = np.random.default_rng(1)
        few = np.zeros((16, 16, 4), np.uint8)
        few[4:12, 4:12] = (200, 30, 30, 255)
        few[0, 0] = (9, 9, 9, 0)  # transparent with a colour: folded to 0 outside fx/
        many = rng.integers(0, 256, (32, 32, 4), dtype=np.uint8)
        many[..., 3] = 255
        pal = Image.new("P", (16, 16))
        pal.putpalette([0, 0, 0, 10, 200, 10, 250, 250, 0, 7, 7, 7] + [0] * (252 * 3))
        pal.putdata([1] * 100 + [0] * 100 + [2] * 50 + [3] * 6)
        out = io.BytesIO()
        pal.save(out, "PNG", transparency=bytes([0, 255, 128, 0]))  # index 0 and 3 clear, 2 half
        self.files = {
            "sprite/p/0.0.png": out.getvalue(),
            "fx/p/0.0.png": out.getvalue(),
            "hd.txt": b"scale 4\nimage sprite/a/0.0.png palettes/p.pal\n",
            "palettes/p.pal": bytes(range(256)) * 3,
            "sprite/a/0.0.png": png(few, {"hdbox": "-8 -12 112 108 tool words"}),
            "sprite/a/0.1.png": png(few, mode="P"),
            "sprite/a/0.2.png": png(few),  # the same bytes as 0.0 without the tEXt: its own blob
            "sprite/b/0.0.png": png(few, {"hdbox": "-8 -12 112 108 tool words"}),  # the same as a/0.0: shares its blob
            "fx/a/0.0.png": png(few, {"hdfx": "0 0 16 16"}),
            "tile/l1/5.png": png(many),
            "held/c057/render.json": b'{"sprites": {}}',
        }
        for name, data in self.files.items():
            path = os.path.join(self.src, *name.split("/"))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(data)
        self.pak = os.path.join(self.root, "pack.hdpk")
        hdpak.main(["build", self.src, self.pak, "-j", "2"])

    def tearDown(self):
        self.tmp.cleanup()

    def open(self):
        return hdpak.Pak(self.pak)

    def test_build_round_trips_every_file(self):
        pak = self.open()
        self.assertEqual(set(pak.entries), set(self.files))
        for name, data in self.files.items():
            got = pak.file_bytes(name)
            if name.endswith(".png"):
                want = pixels(data).copy()
                if not name.startswith("fx/"):
                    want[want[..., 3] == 0] = 0
                np.testing.assert_array_equal(pixels(got), want, name)
            else:
                self.assertEqual(got, data, name)

    def test_codecs_meta_and_shared_blobs(self):
        pak = self.open()
        e = pak.entries
        self.assertEqual(e["sprite/a/0.0.png"].codec, hdpak.IMG8)
        self.assertEqual(e["tile/l1/5.png"].codec, hdpak.STORED, "more than 256 colours: the PNG as it is")
        self.assertEqual(e["hd.txt"].codec in (hdpak.ZLIB, hdpak.STORED), True)
        self.assertEqual(hdpak.parse_meta(e["sprite/a/0.0.png"].meta), [("hdbox", "-8 -12 112 108 tool words")])
        self.assertEqual(hdpak.parse_meta(e["fx/a/0.0.png"].meta), [("hdfx", "0 0 16 16")])
        self.assertEqual(e["sprite/a/0.0.png"].offset, e["sprite/b/0.0.png"].offset)
        # fx keeps transparent colours exactly
        w, h, argb = hdpak.decode_img8(__import__("zlib").decompress(pak.blob(e["fx/a/0.0.png"])))
        self.assertEqual(int(argb[0, 0]), 0x00090909)

    def test_spot_rewrite_keeps_the_rest_and_the_old_state_readable(self):
        before = os.path.getsize(self.pak)
        red = np.zeros((16, 16, 4), np.uint8)
        red[...] = (0, 0, 255, 255)
        new = os.path.join(self.root, "new.png")
        with open(new, "wb") as f:
            f.write(png(red))
        hdpak.main(["put", self.pak, f"sprite/a/0.1.png={new}"])
        pak = self.open()
        np.testing.assert_array_equal(pixels(pak.file_bytes("sprite/a/0.1.png")), red)
        self.assertEqual(len(pak.entries), len(self.files))
        self.assertGreater(os.path.getsize(self.pak), before)
        self.assertLess(os.path.getsize(self.pak) - before, 4096, "only the new blob and index are appended")
        hdpak.main(["rm", self.pak, "sprite/a/0.2.png"])
        hdpak.main(["rm", self.pak, "--prefix", "held/"])
        pak = self.open()
        self.assertNotIn("sprite/a/0.2.png", pak.entries)
        self.assertNotIn("held/c057/render.json", pak.entries)
        hdpak.main(["compact", self.pak])
        pak = self.open()
        np.testing.assert_array_equal(pixels(pak.file_bytes("sprite/a/0.1.png")), red)
        self.assertEqual(pak.file_bytes("hd.txt"), self.files["hd.txt"])
        for jobs in ("1", "3"):
            with self.assertRaises(SystemExit) as done:
                hdpak.main(["verify", self.pak, "-j", jobs])
            self.assertEqual(done.exception.code, 0)

    def test_verify_finds_damage_and_extract_writes_the_folder_back(self):
        out = os.path.join(self.root, "out")
        hdpak.main(["extract", self.pak, out, "-j", "3"])
        for name, data in self.files.items():
            with open(os.path.join(out, *name.split("/")), "rb") as f:
                got = f.read()
            if name.endswith(".png"):
                want = pixels(data).copy()
                if not name.startswith("fx/"):
                    want[want[..., 3] == 0] = 0
                np.testing.assert_array_equal(pixels(got), want, name)
            else:
                self.assertEqual(got, data, name)
        e = self.open().entries["tile/l1/5.png"]
        with open(self.pak, "r+b") as f:
            f.seek(e.offset + e.stored // 2)
            byte = f.read(1)
            f.seek(e.offset + e.stored // 2)
            f.write(bytes([byte[0] ^ 0xFF]))
        with self.assertRaises(SystemExit) as done:
            hdpak.main(["verify", self.pak, "-j", "2"])
        self.assertEqual(done.exception.code, 1)

    def test_a_torn_header_falls_back_to_the_previous_index(self):
        hdpak.main(["rm", self.pak, "hd.txt"])  # generation 2, in slot B
        with open(self.pak, "r+b") as f:
            f.seek(64 + 20)
            f.write(b"\x7f")
        pak = self.open()
        self.assertIn("hd.txt", pak.entries)

    def test_sync_rewrites_only_what_changed(self):
        with open(os.path.join(self.src, "hd.txt"), "ab") as f:
            f.write(b"silhouette free\n")
        os.remove(os.path.join(self.src, "sprite", "a", "0.2.png"))
        pak = self.open()
        offsets = {n: e.offset for n, e in pak.entries.items()}
        pak.close()
        hdpak.main(["sync", self.pak, self.src, "--delete", "-j", "2"])
        pak = self.open()
        self.assertTrue(pak.file_bytes("hd.txt").endswith(b"silhouette free\n"))
        self.assertNotIn("sprite/a/0.2.png", pak.entries)
        self.assertEqual(pak.entries["tile/l1/5.png"].offset, offsets["tile/l1/5.png"])

    def test_a_patch_holds_only_the_changes_and_tombstones(self):
        with open(os.path.join(self.src, "hd.txt"), "ab") as f:
            f.write(b"memory 512\n")
        os.remove(os.path.join(self.src, "tile", "l1", "5.png"))
        patch = os.path.join(self.root, "patch.hdpk")
        hdpak.main(["build", self.src, patch, "--base", self.pak, "-j", "2"])
        p = hdpak.Pak(patch)
        self.assertEqual(set(p.entries), {"hd.txt", "tile/l1/5.png"})
        self.assertEqual(p.entries["tile/l1/5.png"].codec, hdpak.TOMBSTONE)


if __name__ == "__main__":
    unittest.main()
