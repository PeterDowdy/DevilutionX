# HD pack archives (`.hdpk`): a guide for agents

You can now ship an HD pack as **one file** instead of a folder of about a million PNGs. The engine reads the archive directly, and you can replace single images in place without rewriting the rest.

- Engine: DevilutionX fork, branch `diablo/hd-pack` (PR into `hd-layer`). The reader is `Source/engine/render/hd_archive.{hpp,cpp}`, wired into `hd.cpp`.
- Tool: `tools/hdpak.py` in the same branch. It needs Python 3.10+; building from PNGs also needs Pillow and numpy. The pixel-hd-studio backend env has all of them: `uv run --project ~/code/pixel-hd-studio/backend python tools/hdpak.py ...`.
- Nothing about the art changes: same names, same `hd.txt`, same tEXt chunks (`hdbox`, `hdfx`, `hdprop`), same `held/` and `pairs/` layout. The archive is a container for the folder you already make.

## TL;DR

```bash
HDPAK="uv run --project ~/code/pixel-hd-studio/backend python ~/code/DevilutionX-worktrees/hd-pack/tools/hdpak.py"

$HDPAK build  path/to/hd  diablo-hd.hdpk          # pack a folder (follows symlinks)
DEVILUTIONX_HD_PACK=diablo-hd.hdpk devilutionx    # play from the archive (or set "HD Pack" in diablo.ini)

$HDPAK put    diablo-hd.hdpk sprite/monsters/zombie/zombiew/3.5.png=new.png   # replace one image
$HDPAK sync   diablo-hd.hdpk path/to/hd --delete  # rewrite only what changed in the folder
$HDPAK compact diablo-hd.hdpk                      # reclaim the space that old versions left
```

## Choosing the pack

Two settings choose which pack loads. The first one that is set wins:

1. **`DEVILUTIONX_HD_PACK`** (environment variable), when it is set and not empty. The studio's launcher and the tests use this.
2. **`HD Pack` in `diablo.ini`**, under `[Graphics]`. It's an absolute path, or one relative to the config folder (the one holding `diablo.ini`). Empty means no pack. It isn't shown in the settings menu; edit the ini.

```ini
[Graphics]
Upscale=1
HD Pack=/home/peter/diablo-hd-projects/hdpk/diablo-hd.hdpk
```

## How the engine finds files

The pack (from either setting) can be either of two things:

| The pack is | The engine reads |
|---|---|
| **a `.hdpk` file** | that archive only. Nothing loose. This is the shipping setup. |
| **a folder** | 1) the loose file, if there is one; 2) otherwise the folder's `*.hdpk` archives, **later names first** (`1-base.hdpk` < `2-patch.hdpk`). |

So, in folder mode:

- **Loose files always win.** Drop `sprite/.../3.5.png` into the pack folder next to the archive and the next load uses it. This is the quickest way to try a fix without repacking. A loose `hd.txt` replaces the archive's `hd.txt` completely, so keep it in sync if you override it.
- **Later archives win over earlier ones.** A patch archive (`build --base`, below) can replace entries, and its **tombstones** remove entries from the archives before it.
- `held <dir>` / `pairs <dir>` lines in `hd.txt` with a **relative** path are looked up in the archives under that path (e.g. `held/c057/render.json`). An **absolute** path, or `DEVILUTIONX_HD_HELD` / `DEVILUTIONX_HD_PAIRS` set to an absolute path, stays loose-only, exactly as before.
- Every name the engine asks for is the same pack-relative path as before: `hd.txt`, `palettes/...`, `sprite/<path>[@swap]/<group>.<frame>.png`, `tile/...`, `prop/...`, `fx/...`, `held/<key>/...`, `pairs/<key>/...`.

> [!IMPORTANT]
> A running game keeps the archive index it opened. After `put` / `sync` / `rm`, **restart the game (or reload the pack)** to see the changes. Loose files are picked up live, as before, whenever an image is (re)loaded. Don't `compact` an archive that a running game has open on Windows (the rename fails). On Linux it's safe: the game keeps reading the old file until it reloads.

## Concurrency

- **Reading: any number of threads and processes, at once.** Blobs never change once written, so readers never see a half-written entry, even while a writer is appending.
  - The engine reads with positional reads (`pread`; `ReadFile` at an offset on Windows), with no lock between threads. The main thread and the level-loader thread load art in parallel.
  - On Windows the game opens the archive with full sharing, so `put`, `sync` and `compact` work while it runs.
- **Writing: one writer per archive.** Every write command takes an exclusive file lock; a second writer fails with "is being written by another process". This is what makes append-then-switch-header crash-safe.
  - If several agents must write at the same time, give each its own **patch archive** in the pack folder (`2-agentA.hdpk`, `3-agentB.hdpk`). Later names layer over earlier ones, so there's no contention.
  - Or funnel the changes through one writer (one `put` with many `NAME=FILE` arguments, or one `sync`).
- **Tool parallelism:** `build`, `sync`, `verify` and `extract` spread their work over `-j` processes (default: all cores). For `build`/`sync`, encoding runs in parallel and only the appends are sequential. `verify` and `extract` read in parallel, each worker with its own file handle. `compact` is a sequential copy, so it's disk-bound anyway.

## Tool commands

| Command | What it does |
|---|---|
| `build SRC OUT.hdpk [-j N] [--level 9]` | Packs a whole folder into a new archive. Follows symlinks; skips dotfiles, dot-folders, broken links and `*.hdpk`. Writes `OUT.hdpk.part` and renames it when done. |
| `build SRC PATCH.hdpk --base OLD.hdpk` | Writes a **patch**: only the entries that differ from `OLD`, plus tombstones for names that `SRC` no longer has. Ship it next to `OLD` in a folder, named so that it sorts after `OLD`. |
| `sync PAK SRC [--delete] [-j N]` | Brings an existing archive up to date with a folder. Only files whose bytes changed are re-encoded and appended; `--delete` also drops entries the folder no longer has. Commits every 50k changes, so an interrupted sync keeps what it did. |
| `put PAK NAME=FILE ...` | Adds or replaces entries by explicit name. |
| `put PAK --root DIR FILE ...` | The same, with each name being the file's path relative to `DIR` (e.g. `--root build/hd build/hd/sprite/a/0.0.png`). |
| `rm PAK NAME ... [--prefix] [--tombstone]` | Removes entries. `--prefix`: everything under the given prefixes. `--tombstone`: keep a tombstone (use this in a *patch* archive to hide a name from the archives under it). |
| `ls PAK [PREFIX] [-l]` | Lists names. `-l` adds codec, stored size, raw size and tEXt. |
| `cat PAK NAME > out` | Writes one entry's file to stdout. An `img8` image comes back as a PNG with the same pixels and tEXt (not the same bytes). |
| `extract PAK DEST [PREFIX] [-j N]` | Writes entries out as a folder (the inverse of `build`), in parallel. |
| `info PAK` | Entry count, codecs, live bytes, dead space, shared blobs. |
| `verify PAK [-j N]` | Checks every blob's CRC and that it decodes, in parallel. Exits 1 on any failure. |
| `compact PAK` | Rewrites the archive without dead space (to `PAK.compact`, then renamed over `PAK`). |

### Typical workflows

**Studio / packaging (the folder stays the source of truth):**
```bash
# once
$HDPAK build build/hd release/diablo-hd.hdpk
# after each batch of approvals: rewrites only the changed images
$HDPAK sync release/diablo-hd.hdpk build/hd --delete
# now and then, or before a release
$HDPAK compact release/diablo-hd.hdpk && $HDPAK verify release/diablo-hd.hdpk
```

**Shipping an update without re-sending the whole pack:**
```bash
$HDPAK build build/hd updates/2-update-1201.hdpk --base release/1-diablo-hd.hdpk
# players drop 2-update-1201.hdpk into the pack folder next to 1-diablo-hd.hdpk
# (DEVILUTIONX_HD_PACK must point at the folder for layering)
```

**Agent fixing one frame:**
```bash
$HDPAK put pack.hdpk sprite/monsters/zombie/zombiew/3.5.png=/tmp/fixed.png
# or, without touching the archive at all, if the game runs on the folder:
cp /tmp/fixed.png <packfolder>/sprite/monsters/zombie/zombiew/3.5.png
```

**Inspecting what's in a pack:**
```bash
$HDPAK ls pack.hdpk held/c057/ | head
$HDPAK ls -l pack.hdpk sprite/plrgfx/warrior/wls/wlsas/0.   # shows hdbox etc.
$HDPAK cat pack.hdpk hd.txt | head
$HDPAK extract pack.hdpk /tmp/look sprite/monsters/zombie/
```

### Python API (for the studio or scripts)

`tools/hdpak.py` is importable (`sys.path.insert(0, ".../tools"); import hdpak`):

```python
pak = hdpak.Pak("pack.hdpk", write=True)    # takes the lock
pak.add(hdpak.encode("sprite/a/0.0.png", png_bytes))
pak.remove("sprite/a/0.1.png")              # or pak.tombstone(name) in a patch
pak.commit()                                # appends the index and flips the header
names = list(pak.entries)                   # name -> Entry(codec, offset, stored, raw, meta, crc, src)
data = pak.file_bytes("hd.txt")
pak.close()
```

For bulk work, use `hdpak.walk(folder)` and `hdpak.pack_files(pak, files, jobs, level, known)`. Those are what `build` and `sync` use, with a multiprocess encoder.

## What's in the format (short version)

The full spec is in the docstring at the top of `tools/hdpak.py`.

- **Two 64-byte header slots** at offsets 0 and 64, each with a generation number and a CRC. Readers take the valid slot with the higher generation.
- **Append-only writes.** A spot rewrite appends the new blob(s) and a new index, fsyncs, then writes the *other* header slot. A crash at any point leaves the previous state readable; a torn header write is detected by its CRC and ignored. Old blobs become dead space until `compact`.
- **Index** (zlib-compressed): directories sorted by name, then entries sorted by leaf name within each directory, plus a string table. The engine binary-searches it in memory, at about 40 bytes per entry, roughly 40 MB for the full million-entry pack. A tool-only tail holds a CRC and a source hash per entry (used by `sync` and `verify`).
- **Codecs:**
  - `img8`: an image of ≤256 colours, stored as zlib(`w, h, n, n×ARGB, w×h indices`). The engine loads it straight into its 8-bit indexed form with no PNG decode. Fully transparent pixels are stored as 0, except under `fx/`, which keeps them exactly. The engine already folded them this way when loading PNGs, so drawing is unchanged.
  - `stored`: the file's bytes as they are. Used for PNGs of >256 colours, or when `img8` isn't smaller.
  - `zlib`: other files (`hd.txt`, `render.json`, ...).
  - `removed`: a tombstone.
- **tEXt chunks** (`hdbox`, `hdfx`, `hdprop`) are copied into the index, so the engine reads them without opening the image. That matters for props, which are all read when a tileset loads.
- **Identical blobs are stored once.** Entries whose encoded bytes are identical point at the same blob.

## Things to know / gotchas

- **Source names = pack names.** `build` names each entry by its path under `SRC`, with `/` separators. Keep the case the engine uses (lower case, as the studio writes it).
- **`hd.txt` still lists every image** (`image <file> <palette>`). The archive doesn't replace the manifest. An image the manifest lists but no archive or loose file has is logged (`HD: can't load ...: the pack has no such file`) and drawn without HD art, as a missing PNG was before.
- **Only tEXt chunks before IDAT are kept**, as the engine only ever read those.
- **Re-encoding is deterministic per file.** `sync` compares the source bytes' hash, not timestamps, so touching a file without changing it costs nothing.
- **Write archives to the SSD** (e.g. `~/diablo-hd-projects/hdpk/`), not `/mnt/scratch`, which is a spinning disk shared with other jobs.
- **Big builds are I/O-bound.** The full pack is ~954k source files, and the `held/` sources live on `/mnt/scratch`. Expect the first `build` to take a long time; every later `sync` only re-encodes what changed, though it still reads every source file to hash it.
- **Line endings:** `hdpak.py` is plain LF Python; engine sources stay CRLF (see the repo notes).

## Measured on the live pack

Measured 2026-11-27 on `~/diablo-hd-projects/diablo/build/hd`:

- **The folder**: 954,444 files and 7.1 GB, of which 707k are `held/`, 193k `sprite/`, 45k `fx/`, 5.6k `tile/` and 3.8k `prop/`. 247k entries are symlinks.
- **Sprite and held frames** are paletted PNGs (98%+). As `img8` they come to **~0.85× the PNG bytes**, and they load without a PNG decode.
- **Tiles and props** are RGBA with more than 256 colours, so they're stored as the PNG they are (no gain, no loss).
- **A subset** (town/L1/L2 tiles and props, warrior `wls`, towners, 8 monsters, `held/c064`): 44,243 files and 466 MB became **one 392 MB file**. 4,243 entries share a blob with another, and the build took 1 min 20 s with 14 workers.
- **In game**: the WarriorLevel1to2 timedemo was run on that subset once as a folder and once as an archive. **119 of 122 dumped HD frames are pixel-identical.** The other 3 differ only in the in-game menu's pentagram, which animates by wall-clock time, not in any art.
- **In-game performance, full pack**: the WarriorLevel1to2 timedemo (town, then levels 1-2), headless, runs alternated, warm disk cache. Source: `DEVILUTIONX_HD_MEMLOG` and the timedemo's own fps line.

  | | folder | archive |
  |---|---|---|
  | timedemo fps (2 runs) | 36.1, 38.2 | 36.4, 40.3 |
  | pack open (Init) | 268 / 236 ms | 272 / 267 ms |
  | images loaded | 2,650 | 2,650 |
  | total image load time | 3.09 / 2.71 s | **1.49 / 1.47 s** |
  | mean per image | ~1.1 ms | **~0.56 ms** |
  | longest single load | 135 / 123 ms | 131 / 128 ms |
  | peak HD image memory | 557 MB | 557 MB |

  Loading from the archive is **about twice as fast per image**, because img8 skips the PNG decode and there's no per-file open. Frame rate is the same or slightly better, and the open time is the same: it's dominated by parsing `hd.txt`'s 247k `image` lines, not by the archive. The longest single load (~130 ms, the same both ways) is a large full-colour picture, decoded as a PNG in both modes. The folder runs read from a warm page cache; a cold start on the folder pays a million file opens, much of it on the spinning disk, while the archive is one file.
- **Index at full scale** (a synthetic archive of 1,000,000 held-style names): the engine opens it in **21 ms**, the whole test process sits at **48 MB RSS**, and a lookup takes **0.18 µs**. `hdpak.py` needs a few seconds to read a million-entry index on each `put`/`rm`/`sync`.
- **The full pack**: 954,435 files and 7.1 GB became **one 4.91 GB file** (0.69×), `~/diablo-hd-projects/hdpk/diablo-hd.hdpk`.
  - 947k entries are img8, 7k are stored PNGs (tiles and props), and 114 are zlib.
  - **228k entries share a blob with another** (copies across subtypes and items). The index is ~16 MB on disk.
  - `build -j 14`: **12.6 min**, written to the SSD. Its source `held/` frames are read from `/mnt/scratch`.
  - `verify -j 14`: **7 min, 0 bad**. That's 170 GB of pixels inflated, on a machine at load ~30.
