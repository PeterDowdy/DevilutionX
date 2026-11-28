#include "engine/render/hd_archive.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

#if defined(_WIN32)
#define DVL_HD_ARCHIVE_WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__unix__) || defined(__APPLE__)
#define DVL_HD_ARCHIVE_PREAD
#include <fcntl.h>
#include <unistd.h>
#endif

#ifdef USE_SDL3
#include <SDL3/SDL.h>
#else
#include <SDL.h>
#endif

#include <zlib.h>

#include "utils/file_util.h"
#include "utils/log.hpp"

namespace devilution::hd {

namespace {

constexpr char Magic[8] = { 'H', 'D', 'P', 'K', '\r', '\n', '\x1a', '\n' };
constexpr uint32_t Version = 1;
constexpr size_t SlotSize = 64;
constexpr size_t DirSize = 16;
constexpr size_t EntrySize = 32;
constexpr uint32_t MostIndexBytes = 1U << 30;

uint16_t Le16(const uint8_t *p)
{
	return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t Le32(const uint8_t *p)
{
	return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t Le64(const uint8_t *p)
{
	return static_cast<uint64_t>(Le32(p)) | (static_cast<uint64_t>(Le32(p + 4)) << 32);
}

bool Inflate(const uint8_t *in, size_t inSize, std::vector<uint8_t> &out, size_t rawSize)
{
	out.resize(rawSize);
	uLongf size = static_cast<uLongf>(rawSize);
	const int result = uncompress(out.data(), &size, in, static_cast<uLong>(inSize));
	return result == Z_OK && size == rawSize;
}

} // namespace

/**
 * Positional reads: each read says where it reads, nothing is shared between reads, so threads read at once without a lock (and a
 * read never sees another's buffered bytes: the header is read anew by Unchanged).
 */
class Archive::File {
public:
	~File()
	{
#if defined(DVL_HD_ARCHIVE_WIN32)
		if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#elif defined(DVL_HD_ARCHIVE_PREAD)
		if (fd_ >= 0) close(fd_);
#else
		if (rw_ != nullptr) {
#ifdef USE_SDL3
			SDL_CloseIO(rw_);
#else
			SDL_RWclose(rw_);
#endif
		}
#endif
	}

	bool Open(const std::string &path)
	{
#if defined(DVL_HD_ARCHIVE_WIN32)
		const auto wide = ToWideChar(path);
		if (wide == nullptr) return false;
		handle_ = CreateFileW(wide.get(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		return handle_ != INVALID_HANDLE_VALUE;
#elif defined(DVL_HD_ARCHIVE_PREAD)
		int flags = O_RDONLY;
#ifdef O_CLOEXEC
		flags |= O_CLOEXEC;
#endif
		fd_ = open(path.c_str(), flags);
		return fd_ >= 0;
#else
#ifdef USE_SDL3
		rw_ = SDL_IOFromFile(path.c_str(), "rb");
#else
		rw_ = SDL_RWFromFile(path.c_str(), "rb");
#endif
		return rw_ != nullptr;
#endif
	}

	bool ReadAt(uint64_t offset, void *out, size_t size)
	{
		auto *dst = static_cast<uint8_t *>(out);
#if defined(DVL_HD_ARCHIVE_WIN32)
		while (size > 0) {
			OVERLAPPED at = {};
			at.Offset = static_cast<DWORD>(offset);
			at.OffsetHigh = static_cast<DWORD>(offset >> 32);
			const DWORD want = static_cast<DWORD>(std::min<size_t>(size, 1U << 30));
			DWORD got = 0;
			if (ReadFile(handle_, dst, want, &got, &at) == 0 || got == 0) return false;
			dst += got;
			offset += got;
			size -= got;
		}
		return true;
#elif defined(DVL_HD_ARCHIVE_PREAD)
		if (offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) return false; // a 32-bit off_t
		while (size > 0) {
			const ssize_t got = pread(fd_, dst, size, static_cast<off_t>(offset));
			if (got <= 0) return false;
			dst += got;
			offset += static_cast<uint64_t>(got);
			size -= static_cast<size_t>(got);
		}
		return true;
#else
		const std::lock_guard<std::mutex> lock(mutex_);
#ifdef USE_SDL3
		if (SDL_SeekIO(rw_, static_cast<Sint64>(offset), SDL_IO_SEEK_SET) < 0) return false;
		return SDL_ReadIO(rw_, dst, size) == size;
#else
		if (SDL_RWseek(rw_, static_cast<Sint64>(offset), RW_SEEK_SET) < 0) return false;
		return SDL_RWread(rw_, dst, 1, size) == size;
#endif
#endif
	}

private:
#if defined(DVL_HD_ARCHIVE_WIN32)
	HANDLE handle_ = INVALID_HANDLE_VALUE;
#elif defined(DVL_HD_ARCHIVE_PREAD)
	int fd_ = -1;
#else
#ifdef USE_SDL3
	SDL_IOStream *rw_ = nullptr;
#else
	SDL_RWops *rw_ = nullptr;
#endif
	std::mutex mutex_;
#endif
};

Archive::Archive() = default;
Archive::~Archive() = default;

bool Archive::ReadAt(uint64_t offset, void *out, size_t size) const
{
	return file_ != nullptr && file_->ReadAt(offset, out, size);
}

bool Archive::ReadHeader(uint32_t &generation, uint64_t &indexOffset, uint32_t &indexStored, uint32_t &indexRaw) const
{
	uint8_t head[SlotSize * 2];
	if (!ReadAt(0, head, sizeof(head))) return false;
	bool found = false;
	for (int i = 0; i < 2; ++i) {
		const uint8_t *s = head + i * SlotSize;
		if (std::memcmp(s, Magic, sizeof(Magic)) != 0 || Le32(s + 8) != Version) continue;
		if (static_cast<uint32_t>(crc32(0, s, 60)) != Le32(s + 60)) continue; // a slot being written
		const uint32_t gen = Le32(s + 12);
		if (found && gen <= generation) continue;
		found = true;
		generation = gen;
		indexOffset = Le64(s + 16);
		indexStored = Le32(s + 24);
		indexRaw = Le32(s + 28);
	}
	return found;
}

bool Archive::Open(const std::string &path)
{
	path_ = path;
	file_ = std::make_unique<File>();
	if (!file_->Open(path)) {
		file_ = nullptr;
		LogError("HD: can't open {}", path);
		return false;
	}
	uint32_t stored = 0, raw = 0;
	if (!ReadHeader(generation_, indexOffset_, stored, raw)) {
		LogError("HD: {} is not an HD pack archive (version {})", path, Version);
		return false;
	}
	if (stored == 0) return true; // empty
	if (raw < 16 || raw > MostIndexBytes || stored > MostIndexBytes) {
		LogError("HD: {}: bad index", path);
		return false;
	}
	std::vector<uint8_t> packed(stored);
	if (!ReadAt(indexOffset_, packed.data(), stored) || !Inflate(packed.data(), stored, index_, raw)) {
		LogError("HD: {}: can't read the index", path);
		return false;
	}
	const uint8_t *p = index_.data();
	const uint32_t dirCount = Le32(p);
	entryCount_ = Le32(p + 4);
	stringBytes_ = Le32(p + 8);
	const uint64_t need = 16 + static_cast<uint64_t>(dirCount) * DirSize + static_cast<uint64_t>(entryCount_) * EntrySize + stringBytes_;
	if (need > index_.size()) {
		LogError("HD: {}: bad index", path);
		entryCount_ = 0;
		return false;
	}
	const uint8_t *dirs = p + 16;
	entries_ = dirs + static_cast<size_t>(dirCount) * DirSize;
	strings_ = reinterpret_cast<const char *>(entries_ + static_cast<size_t>(entryCount_) * EntrySize);
	dirs_.reserve(dirCount);
	for (uint32_t i = 0; i < dirCount; ++i) {
		const uint8_t *d = dirs + static_cast<size_t>(i) * DirSize;
		const uint32_t first = Le32(d + 8), count = Le32(d + 12);
		if (static_cast<uint64_t>(first) + count > entryCount_) {
			LogError("HD: {}: bad index", path);
			dirs_.clear();
			entryCount_ = 0;
			return false;
		}
		dirs_.push_back({ String(Le32(d), Le32(d + 4)), first, count });
	}
	LogVerbose("HD: archive {}: {} entries in {} folders", path, entryCount_, dirCount);
	return true;
}

bool Archive::Unchanged() const
{
	if (file_ == nullptr) return false;
	// read the header through a handle of its own: where reads go through SDL, the open one's buffer may still hold what it read
	// before; and after a compact the path names a new file, which is what counts
	Archive fresh;
	fresh.path_ = path_;
	fresh.file_ = std::make_unique<File>();
	if (!fresh.file_->Open(path_)) return false;
	uint32_t generation = 0, stored = 0, raw = 0;
	uint64_t offset = 0;
	return fresh.ReadHeader(generation, offset, stored, raw) && generation == generation_ && offset == indexOffset_;
}

std::string_view Archive::String(uint32_t offset, uint32_t length) const
{
	if (static_cast<uint64_t>(offset) + length > stringBytes_) return {};
	return { strings_ + offset, length };
}

bool Archive::Find(std::string_view name, Entry &entry) const
{
	if (dirs_.empty()) return false;
	const size_t slash = name.rfind('/');
	const std::string_view dir = slash == std::string_view::npos ? std::string_view() : name.substr(0, slash);
	const std::string_view leaf = slash == std::string_view::npos ? name : name.substr(slash + 1);
	const auto d = std::lower_bound(dirs_.begin(), dirs_.end(), dir, [](const Dir &a, std::string_view b) { return a.name < b; });
	if (d == dirs_.end() || d->name != dir) return false;
	const auto leafOf = [this](uint32_t i) {
		const uint8_t *e = entries_ + static_cast<size_t>(i) * EntrySize;
		return String(Le32(e), Le16(e + 4));
	};
	uint32_t lo = d->first, hi = d->first + d->count;
	while (lo < hi) {
		const uint32_t mid = lo + (hi - lo) / 2;
		if (leafOf(mid) < leaf)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo == d->first + d->count || leafOf(lo) != leaf) return false;
	const uint8_t *e = entries_ + static_cast<size_t>(lo) * EntrySize;
	entry.codec = e[6];
	entry.offset = Le64(e + 8);
	entry.stored = Le32(e + 16);
	entry.raw = Le32(e + 20);
	entry.meta = String(Le32(e + 24), Le32(e + 28));
	return true;
}

bool Archive::Read(const Entry &entry, std::vector<uint8_t> &out) const
{
	if (entry.codec == Removed) return false;
	if (entry.codec == Stored) {
		out.resize(entry.stored);
		if (entry.stored == 0 || ReadAt(entry.offset, out.data(), entry.stored)) return true;
		LogError("HD: {}: can't read {} bytes at {}", path_, entry.stored, entry.offset);
		return false;
	}
	if (entry.codec != Zlib && entry.codec != Img8) {
		LogError("HD: {}: an entry of an unknown kind ({})", path_, entry.codec);
		return false;
	}
	if (entry.stored > MostIndexBytes || entry.raw > MostIndexBytes) return false;
	std::vector<uint8_t> packed(entry.stored);
	if (!ReadAt(entry.offset, packed.data(), entry.stored) || !Inflate(packed.data(), packed.size(), out, entry.raw)) {
		LogError("HD: {}: can't read the entry at {}", path_, entry.offset);
		return false;
	}
	return true;
}

std::string Archive::Text(const Entry &entry, std::string_view keyword)
{
	std::string_view meta = entry.meta;
	while (!meta.empty()) {
		const size_t keyEnd = meta.find('\0');
		if (keyEnd == std::string_view::npos) break;
		const size_t valueEnd = meta.find('\0', keyEnd + 1);
		const std::string_view value = meta.substr(keyEnd + 1, valueEnd == std::string_view::npos ? std::string_view::npos : valueEnd - keyEnd - 1);
		if (meta.substr(0, keyEnd) == keyword) return std::string(value);
		if (valueEnd == std::string_view::npos) break;
		meta.remove_prefix(valueEnd + 1);
	}
	return {};
}

void Archive::ListDirectories(std::string_view dir, std::vector<std::string> &out) const
{
	std::string prefix(dir);
	if (!prefix.empty()) prefix += '/';
	auto d = std::lower_bound(dirs_.begin(), dirs_.end(), std::string_view(prefix), [](const Dir &a, std::string_view b) { return a.name < b; });
	for (; d != dirs_.end() && d->name.substr(0, prefix.size()) == prefix; ++d) {
		const std::string_view rest = d->name.substr(prefix.size());
		const std::string_view child = rest.substr(0, rest.find('/'));
		if (child.empty()) continue;
		if (std::find(out.begin(), out.end(), child) == out.end()) out.emplace_back(child);
	}
}

bool ParseImg8(const std::vector<uint8_t> &payload, Img8Image &image)
{
	if (payload.size() < 12) return false;
	const uint8_t *p = payload.data();
	image.width = Le32(p);
	image.height = Le32(p + 4);
	image.colourCount = Le16(p + 8);
	if (image.width == 0 || image.height == 0 || image.width > 65536 || image.height > 65536 || image.colourCount == 0 || image.colourCount > 256) return false;
	const uint64_t pixels = static_cast<uint64_t>(image.width) * image.height;
	if (payload.size() != 12 + static_cast<uint64_t>(image.colourCount) * 4 + pixels) return false;
	image.coloursBytes = p + 12;
	image.indices = image.coloursBytes + static_cast<size_t>(image.colourCount) * 4;
	if (image.colourCount < 256) {
		for (uint64_t i = 0; i < pixels; ++i)
			if (image.indices[i] >= image.colourCount) return false;
	}
	return true;
}

} // namespace devilution::hd
