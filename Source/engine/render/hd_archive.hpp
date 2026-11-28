#pragma once

/**
 * HD pack archives (".hdpk"): the files of an HD pack (hd.txt, palettes, render.json, the images) in one file, under the
 * names they have in a pack folder. tools/hdpak.py writes them and documents the layout; this reads them.
 *
 * Entries are found by name in the index, which is kept in memory as read (about 40 bytes an entry). Blobs are read on
 * demand, with positional reads (pread, or ReadFile at an offset on Windows), so any number of threads read at once without a lock;
 * elsewhere through SDL under a mutex. An archive is written by appending and then switching its header to a new index, so a game that has it open
 * keeps reading the index it opened with, whose blobs stay where they are until the archive is compacted (a new file).
 */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace devilution::hd {

class Archive {
public:
	enum Codec : uint8_t {
		Stored = 0,   ///< the file's bytes
		Zlib = 1,     ///< the file's bytes, a zlib stream
		Img8 = 2,     ///< an image of 256 colours or fewer, a zlib stream (Img8Image)
		Removed = 255 ///< a patch archive's tombstone: the name is gone from the archives under it
	};

	struct Entry {
		uint8_t codec = Stored;
		uint64_t offset = 0;
		uint32_t stored = 0; ///< bytes in the archive
		uint32_t raw = 0;    ///< bytes once inflated
		std::string_view meta; ///< an image's PNG tEXt chunks, "key\0value\0..."
	};

	Archive();
	Archive(const Archive &) = delete;
	Archive &operator=(const Archive &) = delete;
	~Archive();

	/** Opens the archive at ``path`` and reads its index; false (and logs why) when it can't. */
	bool Open(const std::string &path);

	/** Whether the archive on disk still has the index this one read (no writer has switched it since Open). */
	[[nodiscard]] bool Unchanged() const;

	/** The entry named ``name`` ("sprite/monsters/zombie/zombiew/3.5.png"), or false. */
	bool Find(std::string_view name, Entry &entry) const;

	/** The entry's bytes: the file's for Stored and Zlib, the inflated payload for Img8. False (logged) on a read or inflate error. */
	bool Read(const Entry &entry, std::vector<uint8_t> &out) const;

	/** The value of the tEXt chunk ``keyword`` of an image entry, or nothing. */
	[[nodiscard]] static std::string Text(const Entry &entry, std::string_view keyword);

	/** The names of the folders directly under ``dir`` ("held": "c057", "c057~doombringer", ...), added to ``out``. */
	void ListDirectories(std::string_view dir, std::vector<std::string> &out) const;

	[[nodiscard]] const std::string &Path() const { return path_; }
	[[nodiscard]] size_t Size() const { return entryCount_; }

private:
	struct Dir {
		std::string_view name;
		uint32_t first;
		uint32_t count;
	};

	/** An open file read at offsets (hd_archive.cpp: pread, ReadFile, or SDL under a mutex). */
	class File;

	bool ReadAt(uint64_t offset, void *out, size_t size) const;
	bool ReadHeader(uint32_t &generation, uint64_t &indexOffset, uint32_t &indexStored, uint32_t &indexRaw) const;
	[[nodiscard]] std::string_view String(uint32_t offset, uint32_t length) const;

	std::string path_;
	std::unique_ptr<File> file_;
	uint32_t generation_ = 0;
	uint64_t indexOffset_ = 0;
	std::vector<uint8_t> index_;
	std::vector<Dir> dirs_; ///< sorted by name
	const uint8_t *entries_ = nullptr;
	uint32_t entryCount_ = 0;
	const char *strings_ = nullptr;
	uint32_t stringBytes_ = 0;
};

/** An Img8 payload's picture: ``colours`` (0xAARRGGBB) and one index into them per pixel, rows top to bottom. */
struct Img8Image {
	uint32_t width = 0;
	uint32_t height = 0;
	const uint8_t *coloursBytes = nullptr; ///< little-endian u32 each: read with Colour()
	uint16_t colourCount = 0;
	const uint8_t *indices = nullptr;

	[[nodiscard]] uint32_t Colour(size_t i) const
	{
		const uint8_t *b = coloursBytes + i * 4;
		return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) | (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
	}
};

/** Reads an Img8 payload (Archive::Read of an Img8 entry); false when it is malformed. */
bool ParseImg8(const std::vector<uint8_t> &payload, Img8Image &image);

} // namespace devilution::hd
