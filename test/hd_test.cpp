#include "engine/render/hd.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <SDL.h>
#include <gtest/gtest.h>

#include "engine/clx_sprite.hpp"
#include "engine/render/clx_render.hpp"
#include "engine/surface.hpp"
#include "utils/png.h"

namespace devilution {
namespace {

namespace fs = std::filesystem;

// Palette indices: the sprites' own colour, a monster type's swap of it, and a unique monster's on top
constexpr uint8_t Own = 10;
constexpr uint8_t Swapped = 20;
constexpr uint8_t Unique = 30;

/** A CLX list of one 4x4 sprite, every pixel ``Own``. */
std::vector<uint8_t> Sprite()
{
	std::vector<uint8_t> data = { 1, 0, 0, 0, 12, 0, 0, 0, 38, 0, 0, 0, 6, 0, 4, 0, 4, 0 };
	for (int row = 0; row < 4; ++row)
		data.insert(data.end(), { 0xFC, Own, Own, Own, Own });
	return data;
}

std::array<SDL_Color, 256> Palette()
{
	std::array<SDL_Color, 256> colors;
	colors.fill({ 100, 100, 100, 255 });
	colors[Own] = { 200, 50, 50, 255 };
	colors[Swapped] = { 50, 50, 200, 255 };
	colors[Unique] = { 50, 200, 50, 255 };
	return colors;
}

/** One HD image, 8x8 of one colour. */
void WriteArt(const fs::path &pack, const std::string &file, SDL_Color c, std::ofstream &index)
{
	fs::create_directories((pack / file).parent_path());
	SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, 8, 8, 32, SDL_PIXELFORMAT_ARGB8888);
	SDL_FillRect(s, nullptr, SDL_MapRGBA(s->format, c.r, c.g, c.b, 255));
	IMG_SavePNG(s, (pack / file).string().c_str());
	SDL_FreeSurface(s);
	index << "image " << file << " palettes/test.pal\n";
}

SDL_Color At(SDL_Surface *hd, int x, int y)
{
	SDL_Color c;
	SDL_GetRGB(static_cast<uint32_t *>(hd->pixels)[y * (hd->pitch / 4) + x], hd->format, &c.r, &c.g, &c.b);
	return c;
}

void ExpectColor(SDL_Color got, int r, int g, int b, const char *what)
{
	EXPECT_EQ(got.r, r) << what;
	EXPECT_EQ(got.g, g) << what;
	EXPECT_EQ(got.b, b) << what;
}

TEST(HdTest, DrawsTheArtOfEachPaletteSwap)
{
	const fs::path pack = fs::temp_directory_path() / "devilutionx_hd_test";
	const fs::path dump = pack / "dump";
	fs::remove_all(pack);
	fs::create_directories(dump);
	const std::array<SDL_Color, 256> colors = Palette();
	{
		fs::create_directories(pack / "palettes");
		std::ofstream pal(pack / "palettes/test.pal", std::ios::binary);
		for (const SDL_Color &c : colors)
			pal << c.r << c.g << c.b;
		std::ofstream index(pack / "hd.txt");
		index << "scale 2\n";
		WriteArt(pack, "sprite/test/a/0.0.png", { 120, 60, 60 }, index);
		WriteArt(pack, "sprite/test/a@+u/0.0.png", { 60, 60, 120 }, index);
		WriteArt(pack, "sprite/test/b/0.0.png", { 120, 60, 60 }, index);
		WriteArt(pack, "sprite/test/b@swap/0.0.png", { 60, 120, 60 }, index);
		WriteArt(pack, "sprite/test/b@swap+u/0.0.png", { 120, 120, 60 }, index);
	}
	setenv("DEVILUTIONX_HD_PACK", pack.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP", dump.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP_EVERY", "1", 1);
	hd::Init();
	ASSERT_TRUE(hd::Enabled());

	SDL_Surface *pal = SDL_CreateRGBSurfaceWithFormat(0, 32, 8, 8, SDL_PIXELFORMAT_INDEX8);
	SDL_SetPaletteColors(pal->format->palette, colors.data(), 0, 256);
	SDL_FillRect(pal, nullptr, 0);
	hd::SetTarget(pal);
	const Surface out(pal);

	std::array<uint8_t, 256> typeTrn, uniqueTrn, otherTrn;
	for (int i = 0; i < 256; ++i)
		typeTrn[i] = uniqueTrn[i] = otherTrn[i] = static_cast<uint8_t>(i);
	typeTrn[Own] = Swapped;
	otherTrn[Own] = Swapped;
	uniqueTrn[Own] = Swapped;
	uniqueTrn[Swapped] = Unique;
	hd::NameTrn(uniqueTrn.data(), "Monsters\\Monsters\\U");

	std::vector<uint8_t> a = Sprite();
	std::vector<uint8_t> b = Sprite();
	hd::RegisterSpriteList("Test\\A.CL2", 0, ClxSpriteList { a.data() });
	hd::RegisterSpriteList("Test\\B.CL2", 0, ClxSpriteList { b.data() });
	{
		const hd::NamedRecolour swap("Test\\Swap"); // as InitMonsterTRN applies a type's TRN
		ClxApplyTrans(ClxSpriteList { b.data() }, typeTrn.data());
	}

	ClxDrawTRN(out, { 0, 3 }, ClxSpriteList { a.data() }[0], otherTrn.data()); // an unnamed TRN: graded
	ClxDrawTRN(out, { 8, 3 }, ClxSpriteList { a.data() }[0], uniqueTrn.data());
	ClxDraw(out, { 16, 3 }, ClxSpriteList { b.data() }[0]);
	ClxDrawTRN(out, { 24, 3 }, ClxSpriteList { b.data() }[0], uniqueTrn.data());
	hd::Present(nullptr, pal);

	SDL_Surface *hd = IMG_LoadPNG((dump / "hd-00000.png").string().c_str());
	ASSERT_NE(hd, nullptr);
	SDL_Surface *argb = SDL_ConvertSurfaceFormat(hd, SDL_PIXELFORMAT_ARGB8888, 0);
	// Own art graded by live[Swapped] / ref[Own], as before swaps had art
	ExpectColor(At(argb, 2, 2), 30, 60, 240, "a through an unnamed TRN");
	// A swap's art shows it already: drawn as it is, so its pixels must record the swapped indices
	ExpectColor(At(argb, 18, 2), 60, 60, 120, "a through the unique's TRN");
	ExpectColor(At(argb, 34, 2), 60, 120, 60, "b in its type's swap");
	ExpectColor(At(argb, 50, 2), 120, 120, 60, "b in its type's swap, through the unique's TRN");
	SDL_FreeSurface(argb);
	SDL_FreeSurface(hd);
	SDL_FreeSurface(pal);
	fs::remove_all(pack);
}

/** A CLX list of one 4x4 sprite whose left column is transparent: the other three, every pixel ``Own``. */
std::vector<uint8_t> NarrowSprite()
{
	std::vector<uint8_t> data = { 1, 0, 0, 0, 12, 0, 0, 0, 38, 0, 0, 0, 6, 0, 4, 0, 4, 0 };
	for (int row = 0; row < 4; ++row)
		data.insert(data.end(), { 1, 0xFD, Own, Own, Own });
	return data;
}

/** One 8x8 HD image: opaque ``c`` everywhere except a transparent 2x2 hole at (2, 2) when ``hole``. */
void WriteArtWithHole(const fs::path &pack, const std::string &file, SDL_Color c, bool hole, std::ofstream &index)
{
	fs::create_directories((pack / file).parent_path());
	SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, 8, 8, 32, SDL_PIXELFORMAT_ARGB8888);
	for (int y = 0; y < 8; ++y) {
		for (int x = 0; x < 8; ++x) {
			const bool clear = hole && x >= 2 && x < 4 && y >= 2 && y < 4;
			static_cast<uint32_t *>(s->pixels)[y * (s->pitch / 4) + x] = clear ? 0U : (0xFF000000U | (c.r << 16) | (c.g << 8) | c.b);
		}
	}
	IMG_SavePNG(s, (pack / file).string().c_str());
	SDL_FreeSurface(s);
	index << "image " << file << " palettes/test.pal\n";
}

SDL_Surface *LoadFrame(const fs::path &dump)
{
	for (const fs::directory_entry &entry : fs::directory_iterator(dump)) {
		if (entry.path().filename().string().rfind("hd-", 0) != 0) continue;
		SDL_Surface *loaded = IMG_LoadPNG(entry.path().string().c_str());
		if (loaded == nullptr) return nullptr;
		SDL_Surface *argb = SDL_ConvertSurfaceFormat(loaded, SDL_PIXELFORMAT_ARGB8888, 0);
		SDL_FreeSurface(loaded);
		return argb;
	}
	return nullptr;
}

constexpr SDL_Color Background = { 100, 100, 100, 255 };
constexpr SDL_Color FrontArt = { 120, 60, 60, 255 };
constexpr SDL_Color BackArt = { 60, 120, 60, 255 };
constexpr uint8_t BackgroundIndex = 5;

/**
 * Draws, on a surface ``width`` wide, a sprite whose left column is transparent (at lo pixels x 8..11, y 4..7), and one over another
 * sprite with art of its own (x 16..19, y 4..7), in a pack that is ``free`` or not. The art of the first covers the whole sprite,
 * the art of the front sprite of the second has a hole over its pixel (17, 5). ``after`` runs before the frame is presented.
 * Returns the HD frame; the PalSurface comes back in ``palOut``.
 */
SDL_Surface *DrawFreeScenario(const std::string &name, bool free, int width, SDL_Surface **palOut, const std::function<void(SDL_Surface *)> &after = {})
{
	const fs::path pack = fs::temp_directory_path() / ("devilutionx_hd_" + name);
	const fs::path dump = pack / "dump";
	fs::remove_all(pack);
	fs::create_directories(dump);
	const std::array<SDL_Color, 256> colors = Palette();
	{
		fs::create_directories(pack / "palettes");
		std::ofstream pal(pack / "palettes/test.pal", std::ios::binary);
		for (const SDL_Color &c : colors)
			pal << c.r << c.g << c.b;
		std::ofstream index(pack / "hd.txt");
		index << "scale 2\n";
		if (free) index << "silhouette free\n";
		WriteArtWithHole(pack, "sprite/" + name + "/a/0.0.png", FrontArt, false, index);
		WriteArtWithHole(pack, "sprite/" + name + "/back/0.0.png", BackArt, false, index);
		WriteArtWithHole(pack, "sprite/" + name + "/front/0.0.png", FrontArt, true, index);
	}
	setenv("DEVILUTIONX_HD_PACK", pack.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP", dump.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP_EVERY", "1", 1);
	hd::Init();
	EXPECT_TRUE(hd::Enabled());

	// Not the size of any other test's surface, so that the provenance plane starts clean
	SDL_Surface *pal = SDL_CreateRGBSurfaceWithFormat(0, width, 12, 8, SDL_PIXELFORMAT_INDEX8);
	SDL_SetPaletteColors(pal->format->palette, colors.data(), 0, 256);
	SDL_FillRect(pal, nullptr, BackgroundIndex);
	hd::SetTarget(pal);
	const Surface out(pal);

	// Sprites are found by the address of their pixel data: keep these alive and in place
	static std::vector<uint8_t> narrow, back, front;
	narrow = NarrowSprite();
	back = Sprite();
	front = Sprite();
	hd::RegisterSpriteList(name + "\\A.CL2", 0, ClxSpriteList { narrow.data() });
	hd::RegisterSpriteList(name + "\\Back.CL2", 0, ClxSpriteList { back.data() });
	hd::RegisterSpriteList(name + "\\Front.CL2", 0, ClxSpriteList { front.data() });
	ClxDraw(out, { 8, 7 }, ClxSpriteList { narrow.data() }[0]);
	ClxDraw(out, { 16, 7 }, ClxSpriteList { back.data() }[0]);
	ClxDraw(out, { 16, 7 }, ClxSpriteList { front.data() }[0]);
	if (after) after(pal);
	hd::Present(nullptr, pal);
	SDL_Surface *frame = LoadFrame(dump);
	fs::remove_all(pack);
	*palOut = pal;
	return frame;
}

TEST(HdTest, FreeSilhouetteDrawsOutsideTheFootprintAndRevealsWhatWasBehind)
{
	SDL_Surface *pal = nullptr;
	SDL_Surface *frame = DrawFreeScenario("free", true, 40, &pal);
	ASSERT_NE(frame, nullptr);
	// The art covers the transparent left column of the first sprite (lo pixel (8, 4) is HD (16, 8))
	ExpectColor(At(frame, 16, 8), FrontArt.r, FrontArt.g, FrontArt.b, "the art outside the footprint");
	ExpectColor(At(frame, 17, 9), FrontArt.r, FrontArt.g, FrontArt.b, "the art outside the footprint, its last pixel");
	ExpectColor(At(frame, 18, 8), FrontArt.r, FrontArt.g, FrontArt.b, "the art inside the footprint");
	ExpectColor(At(frame, 14, 8), Background.r, Background.g, Background.b, "outside the art's rectangle");
	EXPECT_EQ(static_cast<uint8_t *>(pal->pixels)[4 * pal->pitch + 8], Own) << "the extension takes the nearest pixel's index";
	// The front sprite's hole shows the back sprite's art, not the front sprite's own pixel
	ExpectColor(At(frame, 34, 10), BackArt.r, BackArt.g, BackArt.b, "the hole shows what was behind");
	ExpectColor(At(frame, 35, 11), BackArt.r, BackArt.g, BackArt.b, "the hole shows what was behind, its last pixel");
	ExpectColor(At(frame, 32, 10), FrontArt.r, FrontArt.g, FrontArt.b, "the front art beside the hole");
	SDL_FreeSurface(frame);
	SDL_FreeSurface(pal);
}

TEST(HdTest, WithoutFreeSilhouetteArtStaysInsideTheFootprint)
{
	SDL_Surface *pal = nullptr;
	SDL_Surface *frame = DrawFreeScenario("bound", false, 44, &pal);
	ASSERT_NE(frame, nullptr);
	ExpectColor(At(frame, 16, 8), Background.r, Background.g, Background.b, "no art outside the footprint");
	ExpectColor(At(frame, 18, 8), FrontArt.r, FrontArt.g, FrontArt.b, "the art inside the footprint");
	EXPECT_EQ(static_cast<uint8_t *>(pal->pixels)[4 * pal->pitch + 8], BackgroundIndex) << "the PalSurface is as the game drew it";
	const SDL_Color own = Palette()[Own];
	ExpectColor(At(frame, 34, 10), own.r, own.g, own.b, "the hole shows the sprite's own pixel");
	SDL_FreeSurface(frame);
	SDL_FreeSurface(pal);
}

TEST(HdTest, FreeSilhouetteGivesWayToLaterDraws)
{
	SDL_Surface *pal = nullptr;
	// Draws that don't know the HD layer overwrite an extension pixel and a pixel of the sprite after it has drawn
	SDL_Surface *frame = DrawFreeScenario("later", true, 48, &pal, [](SDL_Surface *s) {
		static_cast<uint8_t *>(s->pixels)[4 * s->pitch + 8] = Unique;
		static_cast<uint8_t *>(s->pixels)[5 * s->pitch + 9] = Unique;
	});
	ASSERT_NE(frame, nullptr);
	const SDL_Color unique = Palette()[Unique];
	ExpectColor(At(frame, 16, 8), unique.r, unique.g, unique.b, "an extension pixel overwritten later");
	ExpectColor(At(frame, 18, 10), unique.r, unique.g, unique.b, "a sprite pixel overwritten later");
	ExpectColor(At(frame, 18, 8), FrontArt.r, FrontArt.g, FrontArt.b, "its neighbour");
	SDL_FreeSurface(frame);
	SDL_FreeSurface(pal);
}

TEST(HdTest, FreeSilhouetteStaysOverFoliage)
{
	SDL_Surface *pal = nullptr;
	// Floor foliage drawn after the sprite (the tile beside it, DrawCell) goes under the sprite's extension pixels but over its own
	SDL_Surface *frame = DrawFreeScenario("foliage", true, 52, &pal, [](SDL_Surface *s) {
		const Surface out(s);
		const hd::PillarScope pillar(out, { 0, 11 }, 0, 1);
		const hd::FoliageScope foliage;
		const uint8_t art = Unique;
		const int points[2][2] = { { 8, 4 }, { 9, 5 } };
		for (const auto &pt : points) {
			uint8_t *dst = static_cast<uint8_t *>(s->pixels) + pt[1] * s->pitch + pt[0];
			*dst = Unique;
			hd::Mark(dst, &art, 1);
		}
	});
	ASSERT_NE(frame, nullptr);
	const SDL_Color unique = Palette()[Unique];
	ExpectColor(At(frame, 16, 8), FrontArt.r, FrontArt.g, FrontArt.b, "an extension pixel stays over foliage");
	ExpectColor(At(frame, 18, 10), unique.r, unique.g, unique.b, "foliage still covers the sprite's own pixel");
	EXPECT_EQ(static_cast<uint8_t *>(pal->pixels)[4 * pal->pitch + 8], Own) << "the extension's index is put back";
	SDL_FreeSurface(frame);
	SDL_FreeSurface(pal);
}

TEST(HdTest, TrackingBelongsToTheThreadThatDraws)
{
	// The loading thread draws art into surfaces of its own (the virtual gamepad's potions) while the main thread has a tracked
	// draw open or is presenting; its draws must not be recorded in the main thread's provenance plane
	const fs::path pack = fs::temp_directory_path() / "devilutionx_hd_threads";
	fs::remove_all(pack);
	fs::create_directories(pack / "palettes");
	{
		const std::array<SDL_Color, 256> colors = Palette();
		std::ofstream pal(pack / "palettes/test.pal", std::ios::binary);
		for (const SDL_Color &c : colors)
			pal << c.r << c.g << c.b;
		std::ofstream index(pack / "hd.txt");
		index << "scale 2\n";
	}
	setenv("DEVILUTIONX_HD_PACK", pack.string().c_str(), 1);
	unsetenv("DEVILUTIONX_HD_DUMP");
	hd::Init();
	ASSERT_TRUE(hd::Enabled());

	const std::array<SDL_Color, 256> colors = Palette();
	SDL_Surface *pal = SDL_CreateRGBSurfaceWithFormat(0, 52, 12, 8, SDL_PIXELFORMAT_INDEX8);
	SDL_SetPaletteColors(pal->format->palette, colors.data(), 0, 256);
	hd::SetTarget(pal);
	static std::vector<uint8_t> sprite;
	sprite = Sprite();
	const ClxSprite clx = ClxSpriteList { sprite.data() }[0];
	{
		const hd::SpriteScope scope(Surface(pal), { 8, 7 }, clx);
		EXPECT_NE(hd::TrackPlane, nullptr) << "the main thread's draw is tracked";

		bool otherThreadUntracked = false;
		bool otherSurfaceDrawn = false;
		std::thread loader([&]() {
			otherThreadUntracked = hd::TrackPlane == nullptr;
			SDL_Surface *own = SDL_CreateRGBSurfaceWithFormat(0, 8, 8, 8, SDL_PIXELFORMAT_INDEX8);
			SDL_FillRect(own, nullptr, 0);
			{
				const hd::SpriteScope inner(Surface(own), { 1, 5 }, clx);
				ClxDraw(Surface(own), { 1, 5 }, clx);
			}
			otherSurfaceDrawn = static_cast<uint8_t *>(own->pixels)[2 * own->pitch + 2] == Own;
			otherThreadUntracked = otherThreadUntracked && hd::TrackPlane == nullptr;
			SDL_FreeSurface(own);
		});
		loader.join();
		EXPECT_TRUE(otherThreadUntracked) << "another thread starts and ends untracked";
		EXPECT_TRUE(otherSurfaceDrawn) << "and draws into its own surface";
		EXPECT_NE(hd::TrackPlane, nullptr) << "the main thread's draw is still tracked afterwards";
	}
	EXPECT_EQ(hd::TrackPlane, nullptr) << "and ends untracked";
	SDL_FreeSurface(pal);
	fs::remove_all(pack);
}

} // namespace
} // namespace devilution
