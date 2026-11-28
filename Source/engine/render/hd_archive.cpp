#include "engine/render/hd_archive.hpp"

#include <algorithm>
#include <cstring>

#ifdef USE_SDL3
#include <SDL3/SDL.h>
#else
#include <SDL.h>
#endif

#include <zlib.h>

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

Archive::~Archive()
{
	if (file_ == nullptr) return;
#ifdef USE_SDL3
	SDL_CloseIO(static_cast<SDL_IOStream *>(file_));
#else
	SDL_RWclose(static_cast<SDL_RWops *>(file_));
#endif
}

bool Archive::ReadAt(uint64_t offset, void *out, size_t size) const
{
	const std::lock_guard<std::mutex> lock(fileMutex_);
#ifdef USE_SDL3
	auto *io = static_cast<SDL_IOStream *>(file_);
	if (SDL_SeekIO(io, static_cast<Sint64>(offset), SDL_IO_SEEK_SET) < 0) return false;
	return SDL_ReadIO(io, out, size) == size;
#else
	auto *rw = static_cast<SDL_RWops *>(file_);
	if (SDL_RWseek(rw, static_cast<Sint64>(offset), RW_SEEK_SET) < 0) return false;
	return SDL_RWread(rw, out, 1, size) == size;
#endif
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
#ifdef USE_SDL3
	file_ = SDL_IOFromFile(path.c_str(), "rb");
#else
	file_ = SDL_RWFromFile(path.c_str(), "rb");
#endif
	if (file_ == nullptr) {
		LogError("HD: can't open {}: {}", path, SDL_GetError());
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
	// read the header through a handle of its own: the open one's buffer may still hold what it read before
	Archive fresh;
	fresh.path_ = path_;
#ifdef USE_SDL3
	fresh.file_ = SDL_IOFromFile(path_.c_str(), "rb");
#else
	fresh.file_ = SDL_RWFromFile(path_.c_str(), "rb");
#endif
	uint32_t generation = 0, stored = 0, raw = 0;
	uint64_t offset = 0;
	return fresh.file_ != nullptr && fresh.ReadHeader(generation, offset, stored, raw) && generation == generation_ && offset == indexOffset_;
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
