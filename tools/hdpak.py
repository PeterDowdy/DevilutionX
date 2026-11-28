#!/usr/bin/env python3
"""hdpak: the archive format of DevilutionX HD packs (".hdpk").

An HD pack used to be a folder of loose files (hd.txt, palettes, render.json and one PNG per sprite frame,
pillar, prop and effect layer). A full pack is about a million files. An .hdpk archive holds the same
files under the same names in one file, so a pack ships and copies as a single file. Single entries can
be replaced or removed in place ("spot rewrites") without rewriting the rest. The engine reads the
archive directly (see Source/engine/render/hd_archive.hpp); loose files in the pack folder still win
over the archive, so art can be dropped in without repacking.

Layout (little-endian):

    0    header slot A (64 bytes)
    64   header slot B (64 bytes)
    128  blobs and indexes, appended

    slot:  magic "HDPK\\r\\n\\x1a\\n" | u32 version (1) | u32 generation | u64 index offset
           | u32 index stored size | u32 index raw size | 28 bytes reserved (0) | u32 CRC-32 of bytes 0..59
           The valid slot (magic, version, CRC) with the highest generation is current. A writer
           appends blobs and a new index, flushes, then writes the other slot with generation + 1,
           so a crash at any point leaves the previous state readable.

    index (zlib):
           u32 dir count | u32 entry count | u32 string bytes | u32 flags (bit 0: the tool section follows)
           dirs    16 bytes each: u32 name offset, u32 name length, u32 first entry, u32 entry count
                   (sorted bytewise by name; a dir's name has no trailing "/"; files at the top: "")
           entries 32 bytes each: u32 leaf offset, u16 leaf length, u8 codec, u8 flags (0),
                   u64 blob offset, u32 stored size, u32 raw size, u32 meta offset, u32 meta length
                   (sorted bytewise by leaf within their dir; offsets into the string table)
           strings the names and metas (identical strings are stored once)
           tool section (flags bit 0): per entry u32 CRC-32 of the stored blob, 8 bytes BLAKE2b of the
                   source file (sync's change detection, dedupe). The engine ignores it.

    codecs: 0 stored (the file's bytes), 1 zlib (the file's bytes, zlib stream),
            2 img8 (a zlib stream of: u32 width, u32 height, u16 colours (1..256), u16 0,
               colours x u32 0xAARRGGBB, width*height u8 indices, rows top to bottom),
            255 removed (a patch archive's tombstone: the name is gone from the archives under it).
    meta:   an image's PNG tEXt chunks as "key\\0value\\0..." (hdbox, hdfx, hdprop): the engine reads
            them from the index without opening the image.

An img8 image is the PNG's pixels in 32-bit ARGB, with every fully transparent pixel 0 (outside fx/,
which the engine reads exactly; elsewhere it folds them the same way when loading a PNG). PNGs of more
than 256 colours, or that img8 doesn't make smaller, are stored as they are.

Several archives may sit in one pack folder (*.hdpk, read in name order): a later archive's entries
replace an earlier one's, and its tombstones remove them. "build --base" writes such a patch.

Commands (python3 hdpak.py <command> -h for the options):
    build SRC OUT.hdpk            pack a folder (follows symlinks); --base OLD.hdpk writes a patch against OLD
    sync PAK SRC [--delete]       rewrite only the entries whose source file changed; add new ones
    put PAK NAME=FILE ...         add or replace entries; or --root DIR FILE... (names relative to DIR)
    rm PAK NAME ...               remove entries (--prefix: every entry under a prefix)
    ls PAK [PREFIX] [-l]          list entries
    cat PAK NAME                  write an entry's file to stdout (img8 comes back as a PNG with its tEXt)
    extract PAK DEST [PREFIX]     write entries out as a folder
    info PAK                      entry count, codecs, live and dead bytes
    verify PAK                    check every blob's CRC and that it decodes
    compact PAK                   rewrite the archive without dead space (to a new file, then renamed over)

Encoding PNGs needs Pillow and numpy; everything else needs only the standard library.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import os
import struct
import sys
import zlib
from concurrent.futures import ProcessPoolExecutor
from dataclasses import dataclass, field

MAGIC = b"HDPK\r\n\x1a\n"
VERSION = 1
SLOT_SIZE = 64
DATA_START = 128
STORED, ZLIB, IMG8, TOMBSTONE = 0, 1, 2, 255
CODEC_NAMES = {STORED: "stored", ZLIB: "zlib", IMG8: "img8", TOMBSTONE: "removed"}
SLOT = struct.Struct("<8sIIQII28s")  # + u32 crc
DIR = struct.Struct("<IIII")
ENTRY = struct.Struct("<IHBBQIIII")
EXTRA = struct.Struct("<I8s")
IMG8_HEAD = struct.Struct("<IIHH")
assert SLOT.size == 60 and ENTRY.size == 32


@dataclass
class Entry:
    codec: int
    offset: int = 0
    stored: int = 0
    raw: int = 0
    meta: bytes = b""
    crc: int = 0
    src: bytes = bytes(8)


@dataclass
class Encoded:
    """A file ready to append: its blob and what the index says about it."""
    name: str
    codec: int
    blob: bytes
    raw: int
    meta: bytes
    src: bytes
    unchanged: bool = False


def source_hash(data: bytes) -> bytes:
    return hashlib.blake2b(data, digest_size=8).digest()


# ---------------------------------------------------------------- PNG

def png_texts(data: bytes) -> bytes:
    """The tEXt chunks ahead of the pixel data as "key\\0value\\0...", as the engine reads them."""
    out = bytearray()
    pos = 8
    while pos + 8 <= len(data):
        size, kind = struct.unpack(">I4s", data[pos:pos + 8])
        if kind in (b"IDAT", b"IEND"):
            break
        if kind == b"tEXt" and size <= 4096 and b"\0" in data[pos + 8:pos + 8 + size]:
            out += data[pos + 8:pos + 8 + size] + b"\0"
        pos += 12 + size
    return bytes(out)


def parse_meta(meta: bytes) -> list[tuple[str, str]]:
    parts = meta.split(b"\0")
    return [(parts[i].decode("latin-1"), parts[i + 1].decode("latin-1")) for i in range(0, len(parts) - 1, 2)]


def encode_img8(name: str, data: bytes, level: int) -> bytes | None:
    """The img8 blob of a PNG, or None when it has more than 256 colours."""
    import numpy as np
    from PIL import Image
    im = Image.open(io.BytesIO(data))
    if im.mode == "P":  # most of the pack's art: its palette is the colour table, no need to count colours
        blob = _encode_paletted(name, im, level)
        if blob is not None:
            return blob
    im = im.convert("RGBA")
    a = np.asarray(im)
    argb = (a[..., 3].astype(np.uint32) << 24) | (a[..., 0].astype(np.uint32) << 16) \
        | (a[..., 1].astype(np.uint32) << 8) | a[..., 2].astype(np.uint32)
    if not name.startswith("fx/"):
        argb[a[..., 3] == 0] = 0
    colours, inverse = np.unique(argb, return_inverse=True)
    if len(colours) > 256:
        return None
    payload = IMG8_HEAD.pack(im.width, im.height, len(colours), 0) + colours.astype("<u4").tobytes() \
        + inverse.astype(np.uint8).tobytes()
    return zlib.compress(payload, level)


def _encode_paletted(name: str, im, level: int) -> bytes | None:
    """The img8 blob of a paletted PNG straight from its palette and tRNS; None to take the general way."""
    import numpy as np
    idx = np.asarray(im)
    if idx.dtype != np.uint8:
        return None
    pal = np.frombuffer(bytes(im.getpalette() or []), np.uint8)
    n = len(pal) // 3
    if n == 0 or (idx.size and int(idx.max()) >= n):
        return None
    pal = pal[:3 * n].reshape(n, 3).astype(np.uint32)
    alpha = np.full(n, 255, np.uint32)
    trns = im.info.get("transparency")
    if isinstance(trns, (bytes, bytearray)):
        alpha[:min(n, len(trns))] = np.frombuffer(bytes(trns), np.uint8)[:n]
    elif isinstance(trns, int) and trns < n:
        alpha[trns] = 0
    elif trns is not None:
        return None
    argb = (alpha << 24) | (pal[:, 0] << 16) | (pal[:, 1] << 8) | pal[:, 2]
    if not name.startswith("fx/"):
        argb[alpha == 0] = 0
    used = np.zeros(n, bool)
    used[idx] = True
    colours, inverse = np.unique(argb[used], return_inverse=True)  # the colours the picture uses, each once
    remap = np.zeros(n, np.uint8)
    remap[used] = inverse.astype(np.uint8)
    payload = IMG8_HEAD.pack(im.width, im.height, len(colours), 0) + colours.astype("<u4").tobytes() \
        + remap[idx].tobytes()
    return zlib.compress(payload, level)


def decode_img8(raw: bytes):
    """(width, height, ARGB uint32 array) of an img8 payload."""
    import numpy as np
    w, h, n, _ = IMG8_HEAD.unpack_from(raw)
    colours = np.frombuffer(raw, "<u4", n, IMG8_HEAD.size)
    idx = np.frombuffer(raw, np.uint8, w * h, IMG8_HEAD.size + 4 * n)
    return w, h, colours[idx].reshape(h, w)


def img8_to_png(raw: bytes, meta: bytes) -> bytes:
    import numpy as np
    from PIL import Image, PngImagePlugin
    w, h, argb = decode_img8(raw)
    rgba = np.empty((h, w, 4), np.uint8)
    rgba[..., 0] = (argb >> 16) & 255
    rgba[..., 1] = (argb >> 8) & 255
    rgba[..., 2] = argb & 255
    rgba[..., 3] = argb >> 24
    info = PngImagePlugin.PngInfo()
    for k, v in parse_meta(meta):
        info.add_text(k, v)
    out = io.BytesIO()
    Image.fromarray(rgba, "RGBA").save(out, "PNG", pnginfo=info, optimize=False)
    return out.getvalue()


def encode(name: str, data: bytes, level: int = 9) -> Encoded:
    src = source_hash(data)
    if name.lower().endswith(".png") and data[:8] == b"\x89PNG\r\n\x1a\n":
        meta = png_texts(data)
        try:
            blob = encode_img8(name, data, level)
        except Exception as e:  # unreadable: stored as it is, the engine reports it when it loads it
            print(f"hdpak: {name}: {e}; stored as it is", file=sys.stderr)
            blob = None
        if blob is not None and len(blob) < len(data):
            return Encoded(name, IMG8, blob, len(zlib.decompress(blob)), meta, src)
        return Encoded(name, STORED, data, len(data), meta, src)
    z = zlib.compress(data, level)
    if len(z) < len(data):
        return Encoded(name, ZLIB, z, len(data), b"", src)
    return Encoded(name, STORED, data, len(data), b"", src)


def _encode_job(job):
    name, path, known_src, level = job
    with open(path, "rb") as f:
        data = f.read()
    if known_src is not None and source_hash(data) == known_src:
        return Encoded(name, 0, b"", 0, b"", known_src, unchanged=True)
    return encode(name, data, level)


# ---------------------------------------------------------------- archive

class Pak:
    """An open archive: ``entries`` by name. Writers hold an exclusive lock until close()."""

    def __init__(self, path: str, create: bool = False, write: bool = False):
        self.path = path
        self.entries: dict[str, Entry] = {}
        self.generation = 0
        self.slot = 1  # so that the first commit writes slot A
        self.dirty = False
        self.blob_index: dict[tuple[int, int, int], tuple[int, int]] = {}  # (crc, stored, codec) -> (offset, raw)
        if create:
            self.f = open(path, "w+b")
            self.f.write(bytes(DATA_START))
        else:
            self.f = open(path, "r+b" if write else "rb")
        if write or create:
            _lock(self.f)
        if not create:
            self._read()
        for e in self.entries.values():
            if e.codec != TOMBSTONE:
                self.blob_index[(e.crc, e.stored, e.codec)] = (e.offset, e.raw)

    # -- reading

    def _read(self):
        self.f.seek(0)
        head = self.f.read(DATA_START)
        best = None
        for i in range(2):
            s = head[i * SLOT_SIZE:(i + 1) * SLOT_SIZE]
            if len(s) < SLOT_SIZE:
                continue
            magic, version, gen, off, stored, raw, _ = SLOT.unpack_from(s)
            (crc,) = struct.unpack_from("<I", s, 60)
            if magic != MAGIC or version != VERSION or crc != zlib.crc32(s[:60]):
                continue
            if best is None or gen > best[1]:
                best = (i, gen, off, stored, raw)
        if best is None:
            raise SystemExit(f"hdpak: {self.path} is not an hdpk archive (version {VERSION})")
        self.slot, self.generation, off, stored, raw = best
        if stored == 0:
            return
        self.f.seek(off)
        index = zlib.decompress(self.f.read(stored))
        if len(index) != raw:
            raise SystemExit(f"hdpak: {self.path}: index size mismatch")
        ndirs, nentries, nstr, flags = struct.unpack_from("<IIII", index)
        pos = 16
        dirs = [DIR.unpack_from(index, pos + i * DIR.size) for i in range(ndirs)]
        pos += ndirs * DIR.size
        recs = [ENTRY.unpack_from(index, pos + i * ENTRY.size) for i in range(nentries)]
        pos += nentries * ENTRY.size
        strings = index[pos:pos + nstr]
        pos += nstr
        extras = [EXTRA.unpack_from(index, pos + i * EXTRA.size) for i in range(nentries)] if flags & 1 else None
        for dname_off, dname_len, first, count in dirs:
            dname = strings[dname_off:dname_off + dname_len].decode("utf-8")
            for i in range(first, first + count):
                loff, llen, codec, _, boff, bstored, braw, moff, mlen = recs[i]
                leaf = strings[loff:loff + llen].decode("utf-8")
                e = Entry(codec, boff, bstored, braw, bytes(strings[moff:moff + mlen]))
                if extras:
                    e.crc, e.src = extras[i]
                self.entries[f"{dname}/{leaf}" if dname else leaf] = e

    def blob(self, e: Entry) -> bytes:
        self.f.seek(e.offset)
        return self.f.read(e.stored)

    def file_bytes(self, name: str) -> bytes:
        """The entry's file as it was packed (img8 as a PNG of the same pixels and tEXt)."""
        e = self.entries[name]
        b = self.blob(e)
        if e.codec == STORED:
            return b
        if e.codec == ZLIB:
            return zlib.decompress(b)
        if e.codec == IMG8:
            return img8_to_png(zlib.decompress(b), e.meta)
        raise KeyError(f"{name} is removed in this archive")

    # -- writing

    def add(self, enc: Encoded):
        crc = zlib.crc32(enc.blob)
        known = self.blob_index.get((crc, len(enc.blob), enc.codec))
        if known is not None and known[1] == enc.raw:
            offset = known[0]  # the same blob is in the archive already: share it
        else:
            self.f.seek(0, os.SEEK_END)
            offset = max(self.f.tell(), DATA_START)
            self.f.write(enc.blob)
            self.blob_index[(crc, len(enc.blob), enc.codec)] = (offset, enc.raw)
        self.entries[enc.name] = Entry(enc.codec, offset, len(enc.blob), enc.raw, enc.meta, crc, enc.src)
        self.dirty = True

    def tombstone(self, name: str):
        self.entries[name] = Entry(TOMBSTONE)
        self.dirty = True

    def remove(self, name: str):
        if self.entries.pop(name, None) is not None:
            self.dirty = True

    def index_bytes(self) -> bytes:
        strings = bytearray()
        table: dict[bytes, int] = {}

        def put(s: bytes) -> int:
            off = table.get(s)
            if off is None:
                off = table[s] = len(strings)
                strings.extend(s)
            return off

        bydir: dict[bytes, list[tuple[bytes, Entry]]] = {}
        for name, e in self.entries.items():
            d, _, leaf = name.encode("utf-8").rpartition(b"/")
            bydir.setdefault(d, []).append((leaf, e))
        dirs = bytearray()
        recs = bytearray()
        extras = bytearray()
        n = 0
        for d in sorted(bydir):
            items = sorted(bydir[d], key=lambda t: t[0])
            dirs += DIR.pack(put(d), len(d), n, len(items))
            for leaf, e in items:
                if len(leaf) > 0xFFFF:
                    raise SystemExit(f"hdpak: name too long: {leaf[:40]!r}...")
                recs += ENTRY.pack(put(leaf), len(leaf), e.codec, 0, e.offset, e.stored, e.raw,
                                   put(e.meta) if e.meta else 0, len(e.meta))
                extras += EXTRA.pack(e.crc, e.src)
                n += 1
        return struct.pack("<IIII", len(bydir), n, len(strings), 1) + dirs + recs + strings + extras

    def commit(self):
        if not self.dirty:
            return
        raw = self.index_bytes()
        stored = zlib.compress(raw, 6)
        self.f.seek(0, os.SEEK_END)
        off = max(self.f.tell(), DATA_START)
        self.f.write(stored)
        self.f.flush()
        os.fsync(self.f.fileno())
        self.generation += 1
        self.slot ^= 1
        s = SLOT.pack(MAGIC, VERSION, self.generation, off, len(stored), len(raw), bytes(28))
        s += struct.pack("<I", zlib.crc32(s))
        self.f.seek(self.slot * SLOT_SIZE)
        self.f.write(s)
        self.f.flush()
        os.fsync(self.f.fileno())
        self.dirty = False

    def close(self):
        self.f.close()

    def __del__(self):
        f = getattr(self, "f", None)
        if f is not None:
            f.close()

    # -- stats

    def live_bytes(self) -> int:
        seen = set()
        total = 0
        for e in self.entries.values():
            if e.codec != TOMBSTONE and e.offset not in seen:
                seen.add(e.offset)
                total += e.stored
        return total


def _lock(f):
    try:
        import fcntl
        fcntl.flock(f.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        raise SystemExit(f"hdpak: {f.name} is being written by another process")
    except ImportError:  # Windows
        import msvcrt
        try:
            msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
        except OSError:
            raise SystemExit(f"hdpak: {f.name} is being written by another process")


# ---------------------------------------------------------------- folder walking

def walk(root: str) -> dict[str, str]:
    """Pack name -> path of every file under ``root`` (symlinks followed; dotfiles and dot folders skipped)."""
    out = {}
    root = os.path.abspath(root)
    for dp, dn, fn in os.walk(root, followlinks=True):
        dn[:] = [d for d in dn if not d.startswith(".")]
        rel = os.path.relpath(dp, root).replace(os.sep, "/")
        for f in fn:
            if f.startswith(".") or f.endswith(".hdpk"):
                continue
            path = os.path.join(dp, f)
            if not os.path.isfile(path):
                print(f"hdpak: skipped {path} (a broken link?)", file=sys.stderr)
                continue
            out[f if rel == "." else f"{rel}/{f}"] = path
    return out


def pack_files(pak: Pak, files: dict[str, str], jobs: int, level: int, known: dict[str, bytes] | None = None,
               commit_every: int | None = 50000) -> tuple[int, int]:
    """Encodes ``files`` (name -> path) into ``pak``, skipping those whose source hash is ``known[name]``.
    Commits every ``commit_every`` changes (None: once at the end), so an interrupted run keeps what it did.
    (changed, unchanged)."""
    known = known or {}
    work = [(name, path, known.get(name), level) for name, path in sorted(files.items())]
    changed = unchanged = 0
    since = 0
    progress = Progress(len(work))
    with ProcessPoolExecutor(max(1, jobs)) as ex:
        for enc in ex.map(_encode_job, work, chunksize=64):
            if enc.unchanged:
                unchanged += 1
            else:
                pak.add(enc)
                changed += 1
                since += 1
                if commit_every and since >= commit_every:
                    pak.commit()
                    since = 0
            progress.step()
    pak.commit()
    progress.done()
    return changed, unchanged


class Progress:
    def __init__(self, total: int):
        self.total, self.n = total, 0
        self.tty = sys.stderr.isatty()

    def step(self):
        self.n += 1
        if self.n % 5000 == 0:
            msg = f"hdpak: {self.n}/{self.total}"
            print(msg, end="\r" if self.tty else "\n", file=sys.stderr, flush=True)

    def done(self):
        if self.tty and self.total >= 5000:
            print(file=sys.stderr)


# ---------------------------------------------------------------- commands

def cmd_build(a):
    files = walk(a.src)
    base = Pak(a.base) if a.base else None
    tmp = a.out + ".part"
    pak = Pak(tmp, create=True)
    known = {n: e.src for n, e in base.entries.items() if e.codec != TOMBSTONE} if base else None
    changed, same = pack_files(pak, files, a.jobs, a.level, known, commit_every=None)  # a .part file: no use committing early
    gone = 0
    if base:
        for name, e in base.entries.items():
            if name not in files and e.codec != TOMBSTONE:
                pak.tombstone(name)
                gone += 1
        pak.dirty = True  # an empty patch is still an archive
        pak.commit()
        base.close()
    pak.commit() if pak.dirty else None
    if not pak.generation:
        pak.dirty = True
        pak.commit()
    pak.close()
    os.replace(tmp, a.out)
    size = os.path.getsize(a.out)
    what = f"{changed} entries" + (f" changed against {a.base}, {gone} removed, {same} the same" if base else "")
    print(f"{a.out}: {what}, {size / 1e6:.1f} MB")


def cmd_sync(a):
    pak = Pak(a.pak, write=True)
    files = walk(a.src)
    known = {n: e.src for n, e in pak.entries.items() if e.codec != TOMBSTONE}
    changed, same = pack_files(pak, files, a.jobs, a.level, known)
    removed = 0
    if a.delete:
        for name in [n for n in pak.entries if n not in files]:
            pak.remove(name)
            removed += 1
        pak.commit()
    pak.close()
    print(f"{a.pak}: {changed} written, {same} unchanged, {removed} removed")


def cmd_put(a):
    pak = Pak(a.pak, write=True)
    pairs = []
    for item in a.items:
        if a.root:
            path = item
            name = os.path.relpath(os.path.abspath(item), os.path.abspath(a.root)).replace(os.sep, "/")
            if name.startswith(".."):
                raise SystemExit(f"hdpak: {item} is not under {a.root}")
        else:
            name, sep, path = item.partition("=")
            if not sep:
                raise SystemExit(f"hdpak: give NAME=FILE (or --root DIR FILE...), not {item!r}")
        pairs.append((name, path))
    for name, path in pairs:
        with open(path, "rb") as f:
            pak.add(encode(name, f.read(), a.level))
    pak.commit()
    pak.close()
    print(f"{a.pak}: {len(pairs)} written")


def cmd_rm(a):
    pak = Pak(a.pak, write=True)
    names = [n for n in pak.entries if any(n.startswith(p) for p in a.names)] if a.prefix else a.names
    n = 0
    for name in names:
        if a.tombstone:
            pak.tombstone(name)
            n += 1
        elif name in pak.entries:
            pak.remove(name)
            n += 1
        else:
            print(f"hdpak: no {name}", file=sys.stderr)
    pak.commit()
    pak.close()
    print(f"{a.pak}: {n} removed")


def cmd_ls(a):
    pak = Pak(a.pak)
    out = sys.stdout
    for name in sorted(pak.entries):
        if a.prefix and not name.startswith(a.prefix):
            continue
        e = pak.entries[name]
        if a.long:
            meta = " ".join(f"{k}={v!r}" for k, v in parse_meta(e.meta))
            out.write(f"{CODEC_NAMES.get(e.codec, e.codec):7s} {e.stored:9d} {e.raw:9d} {name}{'  ' + meta if meta else ''}\n")
        else:
            out.write(name + "\n")


def cmd_cat(a):
    pak = Pak(a.pak)
    if a.name not in pak.entries:
        raise SystemExit(f"hdpak: no {a.name}")
    sys.stdout.buffer.write(pak.file_bytes(a.name))


def cmd_extract(a):
    pak = Pak(a.pak)
    n = 0
    for name, e in sorted(pak.entries.items()):
        if (a.prefix and not name.startswith(a.prefix)) or e.codec == TOMBSTONE:
            continue
        dest = os.path.join(a.dest, *name.split("/"))
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, "wb") as f:
            f.write(pak.file_bytes(name))
        n += 1
    print(f"{n} files written to {a.dest}")


def cmd_info(a):
    pak = Pak(a.pak)
    size = os.path.getsize(a.pak)
    live = pak.live_bytes()
    codecs: dict[int, list[int]] = {}
    for e in pak.entries.values():
        c = codecs.setdefault(e.codec, [0, 0, 0])
        c[0] += 1
        c[1] += e.stored
        c[2] += e.raw
    print(f"{a.pak}: generation {pak.generation}, {len(pak.entries)} entries, {size / 1e6:.1f} MB on disk")
    for codec, (n, stored, raw) in sorted(codecs.items()):
        print(f"  {CODEC_NAMES.get(codec, codec):8s} {n:8d} entries {stored / 1e6:10.1f} MB stored {raw / 1e6:10.1f} MB raw")
    shared = len(pak.entries) - len({e.offset for e in pak.entries.values() if e.codec != TOMBSTONE}) \
        - sum(1 for e in pak.entries.values() if e.codec == TOMBSTONE)
    print(f"  {shared} entries share a blob with another; live blobs {live / 1e6:.1f} MB, "
          f"dead space and indexes {(size - live - DATA_START) / 1e6:.1f} MB (compact reclaims the dead space)")


def cmd_verify(a):
    pak = Pak(a.pak)
    bad = 0
    for name, e in sorted(pak.entries.items()):
        if e.codec == TOMBSTONE:
            continue
        b = pak.blob(e)
        problem = None
        if len(b) != e.stored or zlib.crc32(b) != e.crc:
            problem = "CRC mismatch"
        elif e.codec in (ZLIB, IMG8):
            try:
                raw = zlib.decompress(b)
                if len(raw) != e.raw:
                    problem = "raw size mismatch"
                elif e.codec == IMG8:
                    w, h, n, _ = IMG8_HEAD.unpack_from(raw)
                    if not 1 <= n <= 256 or len(raw) != IMG8_HEAD.size + 4 * n + w * h or (max(raw[IMG8_HEAD.size + 4 * n:], default=0) >= n):
                        problem = "bad img8 payload"
            except zlib.error as ex:
                problem = f"zlib: {ex}"
        if problem:
            bad += 1
            print(f"{name}: {problem}")
    print(f"{a.pak}: {len(pak.entries)} entries, {bad} bad")
    sys.exit(1 if bad else 0)


def cmd_compact(a):
    src = Pak(a.pak, write=True)
    tmp = a.pak + ".compact"
    dst = Pak(tmp, create=True)
    moved: dict[int, int] = {}
    for name in sorted(src.entries):
        e = src.entries[name]
        if e.codec == TOMBSTONE:
            dst.entries[name] = Entry(TOMBSTONE)
            continue
        if e.offset not in moved:
            dst.f.seek(0, os.SEEK_END)
            moved[e.offset] = max(dst.f.tell(), DATA_START)
            dst.f.write(src.blob(e))
        dst.entries[name] = Entry(e.codec, moved[e.offset], e.stored, e.raw, e.meta, e.crc, e.src)
    dst.dirty = True
    dst.commit()
    dst.close()
    before = os.path.getsize(a.pak)
    os.replace(tmp, a.pak)
    src.close()
    print(f"{a.pak}: {before / 1e6:.1f} MB -> {os.path.getsize(a.pak) / 1e6:.1f} MB")


def main(argv=None):
    p = argparse.ArgumentParser(prog="hdpak", description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    jobs = os.cpu_count() or 1

    s = sub.add_parser("build", help="pack a folder into a new archive")
    s.add_argument("src")
    s.add_argument("out")
    s.add_argument("--base", help="write a patch: only what differs from this archive, tombstones for what is gone")
    s.add_argument("-j", "--jobs", type=int, default=jobs)
    s.add_argument("--level", type=int, default=9, help="zlib level (default 9)")
    s.set_defaults(fn=cmd_build)

    s = sub.add_parser("sync", help="bring an archive up to date with a folder, rewriting only what changed")
    s.add_argument("pak")
    s.add_argument("src")
    s.add_argument("--delete", action="store_true", help="also remove entries the folder no longer has")
    s.add_argument("-j", "--jobs", type=int, default=jobs)
    s.add_argument("--level", type=int, default=9)
    s.set_defaults(fn=cmd_sync)

    s = sub.add_parser("put", help="add or replace entries")
    s.add_argument("pak")
    s.add_argument("items", nargs="+", metavar="NAME=FILE")
    s.add_argument("--root", help="the items are files; their names are their paths relative to ROOT")
    s.add_argument("--level", type=int, default=9)
    s.set_defaults(fn=cmd_put)

    s = sub.add_parser("rm", help="remove entries")
    s.add_argument("pak")
    s.add_argument("names", nargs="+")
    s.add_argument("--prefix", action="store_true", help="the names are prefixes: remove every entry under them")
    s.add_argument("--tombstone", action="store_true",
                   help="keep a tombstone (in a patch archive: hides the name in the archives under it)")
    s.set_defaults(fn=cmd_rm)

    s = sub.add_parser("ls", help="list entries")
    s.add_argument("pak")
    s.add_argument("prefix", nargs="?")
    s.add_argument("-l", "--long", action="store_true", help="codec, stored and raw size, tEXt")
    s.set_defaults(fn=cmd_ls)

    s = sub.add_parser("cat", help="write one entry's file to stdout")
    s.add_argument("pak")
    s.add_argument("name")
    s.set_defaults(fn=cmd_cat)

    s = sub.add_parser("extract", help="write entries out as a folder")
    s.add_argument("pak")
    s.add_argument("dest")
    s.add_argument("prefix", nargs="?")
    s.set_defaults(fn=cmd_extract)

    for name, fn, text in (("info", cmd_info, "sizes and codecs"), ("verify", cmd_verify, "check every blob"),
                           ("compact", cmd_compact, "reclaim dead space")):
        s = sub.add_parser(name, help=text)
        s.add_argument("pak")
        s.set_defaults(fn=fn)

    a = p.parse_args(argv)
    a.fn(a)


if __name__ == "__main__":
    main()
