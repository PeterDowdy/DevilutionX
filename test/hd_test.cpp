#include "engine/render/hd.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <SDL.h>
#include <gtest/gtest.h>

#include "engine/clx_sprite.hpp"
#include "engine/render/clx_render.hpp"
#include "engine/surface.hpp"
#include "utils/palette_blending.hpp"
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
		WriteArt(pack, "sprite/test/b@swap+u~bonesaw-the-litch/0.0.png", { 200, 120, 60 }, index);
	}
	setenv("DEVILUTIONX_HD_PACK", pack.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP", dump.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP_EVERY", "1", 1);
	hd::Init();
	ASSERT_TRUE(hd::Enabled());

	SDL_Surface *pal = SDL_CreateRGBSurfaceWithFormat(0, 48, 8, 8, SDL_PIXELFORMAT_INDEX8);
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
	// Two more unique monsters with that same TRN (each loads a copy of its own): one with art that is its alone, one without
	std::array<uint8_t, 256> bonesawTrn = uniqueTrn, stormbaneTrn = uniqueTrn;
	hd::NameTrn(bonesawTrn.data(), "Monsters\\Monsters\\U", "Bonesaw the Litch");
	hd::NameTrn(stormbaneTrn.data(), "Monsters\\Monsters\\U", "Stormbane");

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
	ClxDrawTRN(out, { 32, 3 }, ClxSpriteList { b.data() }[0], bonesawTrn.data());
	ClxDrawTRN(out, { 40, 3 }, ClxSpriteList { b.data() }[0], stormbaneTrn.data());
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
	ExpectColor(At(argb, 66, 2), 200, 120, 60, "b through the TRN of a unique with art of its own");
	ExpectColor(At(argb, 82, 2), 120, 120, 60, "b through the TRN of a unique without: the art its TRN's monsters share");
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

/** One 8x8 HD image: opaque ``c`` except its left two columns (the first pixel of a 4x4 sprite at scale 2), which are transparent. */
void WriteArtLeftClear(const fs::path &pack, const std::string &file, SDL_Color c, std::ofstream &index)
{
	fs::create_directories((pack / file).parent_path());
	SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, 8, 8, 32, SDL_PIXELFORMAT_ARGB8888);
	for (int y = 0; y < 8; ++y)
		for (int x = 0; x < 8; ++x)
			static_cast<uint32_t *>(s->pixels)[y * (s->pitch / 4) + x] = x < 2 ? 0U : (0xFF000000U | (c.r << 16) | (c.g << 8) | c.b);
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
SDL_Surface *DrawFreeScenario(const std::string &name, bool free, int width, SDL_Surface **palOut, const std::function<void(SDL_Surface *)> &after = {}, bool overlap = false)
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
		WriteArtLeftClear(pack, "sprite/" + name + "/overlap/0.0.png", FrontArt, index);
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
	static std::vector<uint8_t> narrow, back, front, later;
	narrow = NarrowSprite();
	back = Sprite();
	front = Sprite();
	later = NarrowSprite();
	hd::RegisterSpriteList(name + "\\A.CL2", 0, ClxSpriteList { narrow.data() });
	hd::RegisterSpriteList(name + "\\Back.CL2", 0, ClxSpriteList { back.data() });
	hd::RegisterSpriteList(name + "\\Front.CL2", 0, ClxSpriteList { front.data() });
	hd::RegisterSpriteList(name + "\\Overlap.CL2", 0, ClxSpriteList { later.data() });
	ClxDraw(out, { 8, 7 }, ClxSpriteList { narrow.data() }[0]);
	ClxDraw(out, { 16, 7 }, ClxSpriteList { back.data() }[0]);
	ClxDraw(out, { 16, 7 }, ClxSpriteList { front.data() }[0]);
	// A later sprite whose art rectangle covers the front sprite's hole (lo pixel (17, 5)), though its own left column, with that pixel, is
	// transparent, in the original and in its art
	if (overlap) ClxDraw(out, { 17, 7 }, ClxSpriteList { later.data() }[0]);
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

TEST(HdTest, ALaterSpritesRectangleLeavesWhatIsBehindAnEarlierOneAlone)
{
	// The town's buildings are drawn after the hero and their art rectangles cover him, with nothing drawn over most of it. Every pixel of
	// such a rectangle used to take the sprite under it as its "ground": the hero's transparent art then blended over his own original
	// pixels (the 1997 warrior behind the new one). Now a draw keeps the record only of the pixels it wrote.
	SDL_Surface *pal = nullptr;
	SDL_Surface *frame = DrawFreeScenario("rect", true, 52, &pal, {}, true);
	ASSERT_NE(frame, nullptr);
	ExpectColor(At(frame, 34, 10), BackArt.r, BackArt.g, BackArt.b, "the hole still shows what was behind the front sprite");
	ExpectColor(At(frame, 35, 11), BackArt.r, BackArt.g, BackArt.b, "the hole still shows what was behind, its last pixel");
	ExpectColor(At(frame, 32, 10), FrontArt.r, FrontArt.g, FrontArt.b, "the front art beside the hole");
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

TEST(HdTest, OffscreenBuffersCarryTheirArtToTheScreen)
{
	// The control panel is drawn once into BottomBuffer and blitted to the screen every frame: its art must survive the copy
	const fs::path pack = fs::temp_directory_path() / "devilutionx_hd_offscreen";
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
		WriteArt(pack, "sprite/panel/a/0.0.png", FrontArt, index);
	}
	setenv("DEVILUTIONX_HD_PACK", pack.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP", dump.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP_EVERY", "1", 1);
	hd::Init();
	ASSERT_TRUE(hd::Enabled());

	SDL_Surface *pal = SDL_CreateRGBSurfaceWithFormat(0, 36, 10, 8, SDL_PIXELFORMAT_INDEX8);
	SDL_SetPaletteColors(pal->format->palette, colors.data(), 0, 256);
	SDL_FillRect(pal, nullptr, BackgroundIndex);
	hd::SetTarget(pal);
	SDL_Surface *buffer = SDL_CreateRGBSurfaceWithFormat(0, 8, 8, 8, SDL_PIXELFORMAT_INDEX8);
	SDL_FillRect(buffer, nullptr, BackgroundIndex);
	SDL_Surface *plain = SDL_CreateRGBSurfaceWithFormat(0, 4, 4, 8, SDL_PIXELFORMAT_INDEX8);
	SDL_FillRect(plain, nullptr, Own);
	hd::TrackOffscreen(buffer);

	static std::vector<uint8_t> sprite;
	sprite = Sprite();
	hd::RegisterSpriteList("Panel\\A.CL2", 0, ClxSpriteList { sprite.data() });
	const Surface out(pal);
	ClxDraw(Surface(buffer), { 2, 5 }, ClxSpriteList { sprite.data() }[0]); // buffer x 2..5, y 2..5
	out.BlitFrom(Surface(buffer), MakeSdlRect(0, 0, 8, 8), { 10, 1 });      // screen x 12..15, y 3..6
	ClxDraw(out, { 24, 6 }, ClxSpriteList { sprite.data() }[0]);             // screen x 24..27, y 3..6
	out.BlitFrom(Surface(plain), MakeSdlRect(0, 0, 4, 4), { 24, 3 });       // ... then covered by a copy from an untracked buffer
	hd::Present(nullptr, pal);
	SDL_Surface *frame = LoadFrame(dump);
	ASSERT_NE(frame, nullptr);
	ExpectColor(At(frame, 24, 6), FrontArt.r, FrontArt.g, FrontArt.b, "the buffer's art after the blit");
	ExpectColor(At(frame, 31, 13), FrontArt.r, FrontArt.g, FrontArt.b, "the buffer's art after the blit, its last pixel");
	ExpectColor(At(frame, 22, 6), Background.r, Background.g, Background.b, "beside it");
	const SDL_Color own = Palette()[Own];
	ExpectColor(At(frame, 50, 8), own.r, own.g, own.b, "a copy from an untracked buffer shows its own pixels");
	hd::ForgetOffscreen(buffer);
	SDL_FreeSurface(frame);
	SDL_FreeSurface(plain);
	SDL_FreeSurface(buffer);
	SDL_FreeSurface(pal);
	fs::remove_all(pack);
}

TEST(HdTest, HellfireMenuArtIsKeptApartFromDiablos)
{
	// LoadPcxSpriteList names a menu file that a Hellfire archive supplies "<name>@hf" (Hellfire's ui_art\heros.pcx has the monk as frame 3,
	// where Diablo's has the new-hero portrait): each name draws its own art only, never the other game's
	const fs::path pack = fs::temp_directory_path() / "devilutionx_hd_menus";
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
		WriteArt(pack, "sprite/ui_art/heros/0.0.png", { 120, 60, 60 }, index);
		WriteArt(pack, "sprite/ui_art/heros@hf/0.0.png", { 60, 120, 60 }, index);
		WriteArt(pack, "sprite/ui_art/title/0.0.png", { 60, 60, 120 }, index);
	}
	setenv("DEVILUTIONX_HD_PACK", pack.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP", dump.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP_EVERY", "1", 1);
	hd::Init();
	ASSERT_TRUE(hd::Enabled());

	SDL_Surface *pal = SDL_CreateRGBSurfaceWithFormat(0, 24, 8, 8, SDL_PIXELFORMAT_INDEX8);
	SDL_SetPaletteColors(pal->format->palette, colors.data(), 0, 256);
	SDL_FillRect(pal, nullptr, 0);
	hd::SetTarget(pal);
	const Surface out(pal);
	std::vector<uint8_t> diablo = Sprite();
	std::vector<uint8_t> hellfire = Sprite();
	std::vector<uint8_t> title = Sprite();
	hd::RegisterSpriteList("ui_art\\heros.pcx", 0, ClxSpriteList { diablo.data() });
	hd::RegisterSpriteList("ui_art\\heros@hf", 0, ClxSpriteList { hellfire.data() });
	hd::RegisterSpriteList("ui_art\\title@hf", 0, ClxSpriteList { title.data() }); // Hellfire's own title, no art of its own
	ClxDraw(out, { 0, 3 }, ClxSpriteList { diablo.data() }[0]);
	ClxDraw(out, { 8, 3 }, ClxSpriteList { hellfire.data() }[0]);
	ClxDraw(out, { 16, 3 }, ClxSpriteList { title.data() }[0]);
	hd::Present(nullptr, pal);

	SDL_Surface *argb = LoadFrame(dump);
	ASSERT_NE(argb, nullptr);
	ExpectColor(At(argb, 2, 2), 120, 60, 60, "Diablo's file draws Diablo's art");
	ExpectColor(At(argb, 18, 2), 60, 120, 60, "Hellfire's file draws Hellfire's art");
	const SDL_Color own = colors[Own];
	ExpectColor(At(argb, 34, 2), own.r, own.g, own.b, "a Hellfire file without art keeps its pixels, not Diablo's art");
	SDL_FreeSurface(argb);
	SDL_FreeSurface(pal);
	fs::remove_all(pack);
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

/** Adds a tEXt chunk to a PNG file, right after its header chunk. */
void AddPngText(const fs::path &file, const std::string &keyword, const std::string &text)
{
	std::ifstream in(file, std::ios::binary);
	std::string png((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	in.close();
	const std::string body = "tEXt" + keyword + std::string(1, '\0') + text;
	uint32_t crc = 0xFFFFFFFFU;
	for (const char ch : body) {
		crc ^= static_cast<uint8_t>(ch);
		for (int bit = 0; bit < 8; ++bit)
			crc = (crc >> 1) ^ ((crc & 1) != 0 ? 0xEDB88320U : 0);
	}
	crc = ~crc;
	const auto size = static_cast<uint32_t>(body.size() - 4);
	std::string chunk;
	for (int shift = 24; shift >= 0; shift -= 8)
		chunk.push_back(static_cast<char>(size >> shift));
	chunk += body;
	for (int shift = 24; shift >= 0; shift -= 8)
		chunk.push_back(static_cast<char>(crc >> shift));
	png.insert(33, chunk); // the 8 bytes of the signature and the 25 of IHDR
	std::ofstream(file, std::ios::binary) << png;
}

/** An effect layer: the two pictures side by side (laid over, added), ``w`` x ``h`` each, every pixel clear but those of ``set`` (x, y, ARGB). */
void WriteEffect(const fs::path &pack, const std::string &file, int w, int h, const std::string &where, const std::vector<std::array<uint32_t, 3>> &set, std::ofstream &index)
{
	fs::create_directories((pack / file).parent_path());
	SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, 2 * w, h, 32, SDL_PIXELFORMAT_ARGB8888);
	SDL_FillRect(s, nullptr, 0);
	for (const auto &px : set)
		static_cast<uint32_t *>(s->pixels)[px[1] * (s->pitch / 4) + px[0]] = px[2];
	IMG_SavePNG(s, (pack / file).string().c_str());
	SDL_FreeSurface(s);
	if (!where.empty()) AddPngText(pack / file, "hdfx", where);
	index << "image " << file << " -\n";
}

constexpr SDL_Color OtherArt = { 60, 120, 60, 255 };

/**
 * A 4x4 sprite at lo pixels x 8..11, y 8..11 with art of one colour and (``withFx``) an effect layer over lo pixels x 6..13, y 4..11, one
 * file pixel per lo pixel: laid over at (6, 4) opaque, (7, 4) half see-through and (9, 9) (on the sprite); added at (8, 4), (10, 10) and
 * (11, 10) (on the sprite), (12, 5). A second sprite with art is there for ``after``, which runs once the first has drawn. The palette has a
 * full-bright entry when ``bright``. Returns the HD frame; ``again`` gets the frame presented after a NewScene.
 */
SDL_Surface *DrawEffectScenario(const std::string &name, bool withFx, bool bright, int width, const std::function<void(SDL_Surface *, ClxSprite)> &after = {}, SDL_Surface **again = nullptr)
{
	const fs::path pack = fs::temp_directory_path() / ("devilutionx_hd_" + name);
	const fs::path dump = pack / "dump";
	fs::remove_all(pack);
	fs::create_directories(dump);
	std::array<SDL_Color, 256> colors = Palette();
	if (bright) colors[255] = { 255, 255, 255, 255 };
	{
		fs::create_directories(pack / "palettes");
		std::ofstream pal(pack / "palettes/test.pal", std::ios::binary);
		for (const SDL_Color &c : colors)
			pal << c.r << c.g << c.b;
		std::ofstream index(pack / "hd.txt");
		index << "scale 2\n";
		WriteArt(pack, "sprite/" + name + "/a/0.0.png", FrontArt, index);
		WriteArt(pack, "sprite/" + name + "/b/0.0.png", OtherArt, index);
		if (withFx) {
			WriteEffect(pack, "fx/" + name + "/a/0.0.png", 8, 8, "-2 -4 8 8",
			    { { 0, 0, 0xFF0A141EU }, { 1, 0, 0x80C86400U }, { 3, 5, 0xFF010203U },
			        { 8 + 2, 0, 0xFF323C46U }, { 8 + 4, 6, 0xFF282828U }, { 8 + 5, 6, 0xFFC8C8C8U }, { 8 + 6, 1, 0xFF323C46U } },
			    index);
		}
	}
	setenv("DEVILUTIONX_HD_PACK", pack.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP", dump.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP_EVERY", "1", 1);
	hd::Init();
	EXPECT_TRUE(hd::Enabled());

	SDL_Surface *pal = SDL_CreateRGBSurfaceWithFormat(0, width, 16, 8, SDL_PIXELFORMAT_INDEX8);
	SDL_SetPaletteColors(pal->format->palette, colors.data(), 0, 256);
	SDL_FillRect(pal, nullptr, BackgroundIndex);
	hd::SetTarget(pal);
	hd::NewScene();
	const Surface out(pal);

	static std::vector<uint8_t> a, b;
	a = Sprite();
	b = Sprite();
	hd::RegisterSpriteList(name + "\\A.CL2", 0, ClxSpriteList { a.data() });
	hd::RegisterSpriteList(name + "\\B.CL2", 0, ClxSpriteList { b.data() });
	ClxDraw(out, { 8, 11 }, ClxSpriteList { a.data() }[0]);
	if (after) after(pal, ClxSpriteList { b.data() }[0]);
	hd::Present(nullptr, pal);
	SDL_Surface *frame = LoadFrame(dump);
	if (again != nullptr) {
		fs::remove_all(dump);
		fs::create_directories(dump);
		hd::NewScene();
		hd::Present(nullptr, pal);
		*again = LoadFrame(dump);
	}
	SDL_FreeSurface(pal);
	fs::remove_all(pack);
	return frame;
}

TEST(HdTest, EffectLayerIsLaidOverAndAddedBeyondTheSprite)
{
	SDL_Surface *again = nullptr;
	SDL_Surface *frame = DrawEffectScenario("fx", true, true, 56, {}, &again);
	ASSERT_NE(frame, nullptr);
	ASSERT_NE(again, nullptr);
	// lo pixel (x, y) is the HD block at (2x, 2y)
	ExpectColor(At(frame, 12, 8), 10, 20, 30, "laid over, opaque, above and left of the sprite");
	ExpectColor(At(frame, 13, 9), 10, 20, 30, "its last pixel");
	ExpectColor(At(frame, 14, 8), 150, 100, 49, "laid over by its alpha");
	ExpectColor(At(frame, 16, 8), 150, 160, 170, "added to the background");
	ExpectColor(At(frame, 24, 10), 150, 160, 170, "added right of the sprite");
	ExpectColor(At(frame, 18, 18), 1, 2, 3, "laid over the sprite's art");
	ExpectColor(At(frame, 20, 20), 160, 100, 100, "added to the sprite's art");
	ExpectColor(At(frame, 22, 20), 255, 255, 255, "added up to white");
	ExpectColor(At(frame, 16, 16), FrontArt.r, FrontArt.g, FrontArt.b, "the art where the layer is clear");
	ExpectColor(At(frame, 18, 8), Background.r, Background.g, Background.b, "the background where the layer is clear");
	// A new scene in which the sprite is not drawn has no layer
	ExpectColor(At(again, 12, 8), Background.r, Background.g, Background.b, "gone with the next scene");
	ExpectColor(At(again, 18, 18), FrontArt.r, FrontArt.g, FrontArt.b, "the art is still there");
	SDL_FreeSurface(frame);
	SDL_FreeSurface(again);
}

TEST(HdTest, WithoutAnEffectLayerNothingChanges)
{
	SDL_Surface *frame = DrawEffectScenario("nofx", false, true, 60);
	ASSERT_NE(frame, nullptr);
	ExpectColor(At(frame, 12, 8), Background.r, Background.g, Background.b, "no layer above the sprite");
	ExpectColor(At(frame, 18, 18), FrontArt.r, FrontArt.g, FrontArt.b, "the art");
	ExpectColor(At(frame, 20, 20), FrontArt.r, FrontArt.g, FrontArt.b, "the art, unlit");
	SDL_FreeSurface(frame);
}

TEST(HdTest, EffectLayerGivesWayToLaterDraws)
{
	// After the sprite: a draw that doesn't know the HD layer changes lo pixel (6, 4), and a sprite with art covers lo pixels x 8..11, y 4..7
	SDL_Surface *frame = DrawEffectScenario("fxlater", true, true, 64, [](SDL_Surface *s, ClxSprite other) {
		static_cast<uint8_t *>(s->pixels)[4 * s->pitch + 6] = Unique;
		ClxDraw(Surface(s), { 8, 7 }, other);
	});
	ASSERT_NE(frame, nullptr);
	const SDL_Color unique = Palette()[Unique];
	ExpectColor(At(frame, 12, 8), unique.r, unique.g, unique.b, "a pixel drawn later without HD art covers the layer");
	ExpectColor(At(frame, 16, 8), OtherArt.r, OtherArt.g, OtherArt.b, "a sprite drawn later covers the layer");
	ExpectColor(At(frame, 14, 8), 150, 100, 49, "the layer beside them");
	ExpectColor(At(frame, 24, 10), 150, 160, 170, "and further off");
	SDL_FreeSurface(frame);
}

TEST(HdTest, EffectLayerFadesWithThePalette)
{
	// No full-bright entry: the palette's brightest channel is 200, so the layer shows at 200 / 248 (206 / 256)
	SDL_Surface *frame = DrawEffectScenario("fxfade", true, false, 68);
	ASSERT_NE(frame, nullptr);
	ExpectColor(At(frame, 12, 8), 8, 16, 24, "laid over, faded");
	ExpectColor(At(frame, 16, 8), 140, 148, 156, "added, faded");
	SDL_FreeSurface(frame);
}

TEST(HdTest, EffectLayerWithoutItsPlaceIsIgnored)
{
	const fs::path pack = fs::temp_directory_path() / "devilutionx_hd_fxbad";
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
		WriteArt(pack, "sprite/fxbad/a/0.0.png", FrontArt, index);
		WriteEffect(pack, "fx/fxbad/a/0.0.png", 4, 4, "", { { 0, 0, 0xFF0A141EU }, { 1, 1, 0xFF0A141EU } }, index);
	}
	setenv("DEVILUTIONX_HD_PACK", pack.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP", dump.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP_EVERY", "1", 1);
	hd::Init();
	ASSERT_TRUE(hd::Enabled());
	SDL_Surface *pal = SDL_CreateRGBSurfaceWithFormat(0, 72, 16, 8, SDL_PIXELFORMAT_INDEX8);
	SDL_SetPaletteColors(pal->format->palette, colors.data(), 0, 256);
	SDL_FillRect(pal, nullptr, BackgroundIndex);
	hd::SetTarget(pal);
	hd::NewScene();
	std::vector<uint8_t> a = Sprite();
	hd::RegisterSpriteList("fxbad\\A.CL2", 0, ClxSpriteList { a.data() });
	ClxDraw(Surface(pal), { 8, 11 }, ClxSpriteList { a.data() }[0]);
	hd::Present(nullptr, pal);
	SDL_Surface *frame = LoadFrame(dump);
	ASSERT_NE(frame, nullptr);
	ExpectColor(At(frame, 16, 16), FrontArt.r, FrontArt.g, FrontArt.b, "the art, with no layer over it");
	ExpectColor(At(frame, 18, 18), FrontArt.r, FrontArt.g, FrontArt.b, "the art, with no layer over it");
	SDL_FreeSurface(frame);
	SDL_FreeSurface(pal);
	fs::remove_all(pack);
}

TEST(HdTest, EffectLayerOfAPaletteSwapIsItsOwnWhenThePackHasOne)
{
	const fs::path pack = fs::temp_directory_path() / "devilutionx_hd_fxswap";
	const fs::path dump = pack / "dump";
	fs::remove_all(pack);
	fs::create_directories(dump);
	std::array<SDL_Color, 256> colors = Palette();
	colors[255] = { 255, 255, 255, 255 };
	{
		fs::create_directories(pack / "palettes");
		std::ofstream pal(pack / "palettes/test.pal", std::ios::binary);
		for (const SDL_Color &c : colors)
			pal << c.r << c.g << c.b;
		std::ofstream index(pack / "hd.txt");
		index << "scale 2\n";
		// a: a body and its swap, each with an effect layer; b: the same, but the swap has none; c: a body with a layer, drawn without its swap
		for (const char *name : { "a", "b", "c" })
			WriteArt(pack, std::string("sprite/fxswap/") + name + "/0.0.png", FrontArt, index);
		WriteArt(pack, "sprite/fxswap/a@swap/0.0.png", OtherArt, index);
		WriteArt(pack, "sprite/fxswap/b@swap/0.0.png", OtherArt, index);
		const std::vector<std::array<uint32_t, 3>> body = { { 0, 0, 0xFF0A141EU } };
		const std::vector<std::array<uint32_t, 3>> own = { { 0, 0, 0xFF8C1E1EU } };
		WriteEffect(pack, "fx/fxswap/a/0.0.png", 8, 8, "-2 -4 8 8", body, index);
		WriteEffect(pack, "fx/fxswap/a@swap/0.0.png", 8, 8, "-2 -4 8 8", own, index);
		WriteEffect(pack, "fx/fxswap/b/0.0.png", 8, 8, "-2 -4 8 8", body, index);
		WriteEffect(pack, "fx/fxswap/c/0.0.png", 8, 8, "-2 -4 8 8", body, index);
	}
	setenv("DEVILUTIONX_HD_PACK", pack.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP", dump.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP_EVERY", "1", 1);
	hd::Init();
	ASSERT_TRUE(hd::Enabled());

	SDL_Surface *pal = SDL_CreateRGBSurfaceWithFormat(0, 64, 16, 8, SDL_PIXELFORMAT_INDEX8);
	SDL_SetPaletteColors(pal->format->palette, colors.data(), 0, 256);
	SDL_FillRect(pal, nullptr, BackgroundIndex);
	hd::SetTarget(pal);
	hd::NewScene();
	const Surface out(pal);

	std::array<uint8_t, 256> typeTrn;
	for (int i = 0; i < 256; ++i)
		typeTrn[i] = static_cast<uint8_t>(i);
	typeTrn[Own] = Swapped;
	std::vector<uint8_t> a = Sprite();
	std::vector<uint8_t> b = Sprite();
	std::vector<uint8_t> c = Sprite();
	hd::RegisterSpriteList("FxSwap\\A.CL2", 0, ClxSpriteList { a.data() });
	hd::RegisterSpriteList("FxSwap\\B.CL2", 0, ClxSpriteList { b.data() });
	hd::RegisterSpriteList("FxSwap\\C.CL2", 0, ClxSpriteList { c.data() });
	{
		const hd::NamedRecolour swap("FxSwap\\Swap"); // as InitMonsterTRN applies a type's TRN
		ClxApplyTrans(ClxSpriteList { a.data() }, typeTrn.data());
		ClxApplyTrans(ClxSpriteList { b.data() }, typeTrn.data());
	}
	ClxDraw(out, { 8, 11 }, ClxSpriteList { a.data() }[0]);
	ClxDraw(out, { 24, 11 }, ClxSpriteList { b.data() }[0]);
	ClxDraw(out, { 40, 11 }, ClxSpriteList { c.data() }[0]);
	hd::Present(nullptr, pal);
	SDL_Surface *frame = LoadFrame(dump);
	ASSERT_NE(frame, nullptr);
	// each layer's first pixel is laid over at lo pixel (x - 2, 4), the HD block at (2x - 4, 8)
	ExpectColor(At(frame, 12, 8), 140, 30, 30, "a swap's own layer replaces the body's");
	ExpectColor(At(frame, 44, 8), 10, 20, 30, "a swap without a layer of its own shows the body's");
	ExpectColor(At(frame, 76, 8), 10, 20, 30, "a body drawn without its swap shows its own layer");
	SDL_FreeSurface(frame);
	SDL_FreeSurface(pal);
	fs::remove_all(pack);
}

/** A CLX list of one 16x4 sprite of which only the right column is drawn. */
std::vector<uint8_t> ThinSprite()
{
	std::vector<uint8_t> data = { 1, 0, 0, 0, 12, 0, 0, 0, 30, 0, 0, 0, 6, 0, 16, 0, 4, 0 };
	for (int row = 0; row < 4; ++row)
		data.insert(data.end(), { 15, 0xFF, Own });
	return data;
}

/** In a free pack: the thin sprite at lo pixels x 8..23, y 4..7 with art over its whole rectangle, and an (empty) effect layer or none. */
SDL_Surface *DrawReachScenario(const std::string &name, bool withFx, int width)
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
		index << "scale 2\nsilhouette free\n";
		WriteArt(pack, "sprite/" + name + "/a/0.0.png", FrontArt, index);
		if (withFx) WriteEffect(pack, "fx/" + name + "/a/0.0.png", 1, 1, "0 0 1 1", {}, index);
	}
	setenv("DEVILUTIONX_HD_PACK", pack.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP", dump.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP_EVERY", "1", 1);
	hd::Init();
	EXPECT_TRUE(hd::Enabled());
	SDL_Surface *pal = SDL_CreateRGBSurfaceWithFormat(0, width, 12, 8, SDL_PIXELFORMAT_INDEX8);
	SDL_SetPaletteColors(pal->format->palette, colors.data(), 0, 256);
	SDL_FillRect(pal, nullptr, BackgroundIndex);
	hd::SetTarget(pal);
	hd::NewScene();
	static std::vector<uint8_t> thin;
	thin = ThinSprite();
	hd::RegisterSpriteList(name + "\\A.CL2", 0, ClxSpriteList { thin.data() });
	ClxDraw(Surface(pal), { 8, 7 }, ClxSpriteList { thin.data() }[0]);
	hd::Present(nullptr, pal);
	SDL_Surface *frame = LoadFrame(dump);
	SDL_FreeSurface(pal);
	fs::remove_all(pack);
	return frame;
}

TEST(HdTest, ArtWithAnEffectLayerIsDrawnWhole)
{
	SDL_Surface *bound = DrawReachScenario("reach", false, 76);
	SDL_Surface *whole = DrawReachScenario("reachfx", true, 80);
	ASSERT_NE(bound, nullptr);
	ASSERT_NE(whole, nullptr);
	// The sprite's own pixels are at lo x 23; lo x 8 is 15 pixels from them
	ExpectColor(At(bound, 44, 8), FrontArt.r, FrontArt.g, FrontArt.b, "art next to the sprite's pixels");
	ExpectColor(At(bound, 16, 8), Background.r, Background.g, Background.b, "art far from the sprite's pixels is left out");
	ExpectColor(At(whole, 44, 8), FrontArt.r, FrontArt.g, FrontArt.b, "art next to the sprite's pixels, with an effect layer");
	ExpectColor(At(whole, 16, 8), FrontArt.r, FrontArt.g, FrontArt.b, "art far from the sprite's pixels shows with an effect layer");
	SDL_FreeSurface(bound);
	SDL_FreeSurface(whole);
}

constexpr SDL_Color TipArt = { 60, 60, 120, 255 };
constexpr SDL_Color CornerArt = { 120, 120, 60, 255 };

/**
 * Art for the 4x4 sprite that covers more than it: 12 x 14 original pixels starting 4 left of and 8 above the sprite's top-left (24 x 28 at
 * scale 2). Clear but for the sprite's own rectangle (``FrontArt``, its top-left pixel ``CornerArt``), a tip above it (the sprite's second
 * column, all 8 rows above), a foot below it (its third column, 2 rows) and one pixel 4 left of its second row, all ``TipArt``.
 */
void WriteBoxedArt(const fs::path &pack, const std::string &file, const std::string &box, std::ofstream &index)
{
	fs::create_directories((pack / file).parent_path());
	SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, 24, 28, 32, SDL_PIXELFORMAT_ARGB8888);
	SDL_FillRect(s, nullptr, 0);
	const auto fill = [s](int x, int y, int w, int h, SDL_Color c) {
		SDL_Rect r = { 2 * x, 2 * y, 2 * w, 2 * h };
		SDL_FillRect(s, &r, SDL_MapRGBA(s->format, c.r, c.g, c.b, 255));
	};
	fill(4, 8, 4, 4, FrontArt);
	fill(4, 8, 1, 1, CornerArt);
	fill(5, 0, 1, 8, TipArt);
	fill(6, 12, 1, 2, TipArt);
	fill(0, 9, 1, 1, TipArt);
	IMG_SavePNG(s, (pack / file).string().c_str());
	SDL_FreeSurface(s);
	if (!box.empty()) AddPngText(pack / file, "hdbox", box);
	index << "image " << file << " palettes/test.pal\n";
}

/**
 * The 4x4 sprite at lo pixels x 8..11, y 8..11 of a surface 16 high, with the boxed art above (its tip at lo x 9, y 0..7; its foot at x 10,
 * y 12..13; its far pixel at (4, 9)), in a pack that is ``free`` or not, drawn into the rows from ``clipTop`` down. ``withFx`` adds the
 * effect layer of DrawEffectScenario; a second sprite with art is there for ``after``, which runs once the first has drawn.
 */
SDL_Surface *DrawBoxScenario(const std::string &name, const std::string &box, bool free, int width, bool withFx = false, int clipTop = 0, const std::function<void(SDL_Surface *, ClxSprite)> &after = {}, SDL_Surface **palOut = nullptr)
{
	const fs::path pack = fs::temp_directory_path() / ("devilutionx_hd_" + name);
	const fs::path dump = pack / "dump";
	fs::remove_all(pack);
	fs::create_directories(dump);
	std::array<SDL_Color, 256> colors = Palette();
	colors[255] = { 255, 255, 255, 255 };
	{
		fs::create_directories(pack / "palettes");
		std::ofstream pal(pack / "palettes/test.pal", std::ios::binary);
		for (const SDL_Color &c : colors)
			pal << c.r << c.g << c.b;
		std::ofstream index(pack / "hd.txt");
		index << "scale 2\n";
		if (free) index << "silhouette free\n";
		WriteBoxedArt(pack, "sprite/" + name + "/a/0.0.png", box, index);
		WriteArt(pack, "sprite/" + name + "/b/0.0.png", OtherArt, index);
		if (withFx) WriteEffect(pack, "fx/" + name + "/a/0.0.png", 8, 8, "-2 -4 8 8", { { 0, 0, 0xFF0A141EU }, { 8 + 6, 1, 0xFF323C46U } }, index);
	}
	setenv("DEVILUTIONX_HD_PACK", pack.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP", dump.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP_EVERY", "1", 1);
	hd::Init();
	EXPECT_TRUE(hd::Enabled());
	SDL_Surface *pal = SDL_CreateRGBSurfaceWithFormat(0, width, 16, 8, SDL_PIXELFORMAT_INDEX8);
	SDL_SetPaletteColors(pal->format->palette, colors.data(), 0, 256);
	SDL_FillRect(pal, nullptr, BackgroundIndex);
	hd::SetTarget(pal);
	hd::NewScene();
	static std::vector<uint8_t> a, b;
	a = Sprite();
	b = Sprite();
	hd::RegisterSpriteList(name + "\\A.CL2", 0, ClxSpriteList { a.data() });
	hd::RegisterSpriteList(name + "\\B.CL2", 0, ClxSpriteList { b.data() });
	ClxDraw(Surface(pal).subregionY(clipTop, 16 - clipTop), { 8, 11 - clipTop }, ClxSpriteList { a.data() }[0]);
	if (after) after(pal, ClxSpriteList { b.data() }[0]);
	hd::Present(nullptr, pal);
	SDL_Surface *frame = LoadFrame(dump);
	if (palOut != nullptr)
		*palOut = pal;
	else
		SDL_FreeSurface(pal);
	fs::remove_all(pack);
	return frame;
}

TEST(HdTest, BoxedArtIsDrawnWholeBeyondTheSprite)
{
	SDL_Surface *pal = nullptr;
	SDL_Surface *frame = DrawBoxScenario("box", "-4 -8 12 14", true, 84, false, 0, {}, &pal);
	ASSERT_NE(frame, nullptr);
	// lo pixel (x, y) is the HD block at (2x, 2y)
	ExpectColor(At(frame, 16, 16), CornerArt.r, CornerArt.g, CornerArt.b, "the sprite's top-left pixel shows its own part of the picture");
	ExpectColor(At(frame, 18, 16), FrontArt.r, FrontArt.g, FrontArt.b, "the art inside the sprite");
	ExpectColor(At(frame, 23, 23), FrontArt.r, FrontArt.g, FrontArt.b, "the art inside the sprite, its last pixel");
	ExpectColor(At(frame, 18, 14), TipArt.r, TipArt.g, TipArt.b, "the tip right above the sprite");
	ExpectColor(At(frame, 18, 0), TipArt.r, TipArt.g, TipArt.b, "the tip 8 pixels above the sprite");
	ExpectColor(At(frame, 19, 1), TipArt.r, TipArt.g, TipArt.b, "the tip, its last pixel");
	ExpectColor(At(frame, 20, 26), TipArt.r, TipArt.g, TipArt.b, "the foot below the sprite");
	ExpectColor(At(frame, 8, 18), TipArt.r, TipArt.g, TipArt.b, "the pixel left of the sprite");
	ExpectColor(At(frame, 16, 0), Background.r, Background.g, Background.b, "beside the tip, where the picture is clear");
	ExpectColor(At(frame, 20, 14), Background.r, Background.g, Background.b, "above the sprite, where the picture is clear");
	ExpectColor(At(frame, 6, 18), Background.r, Background.g, Background.b, "outside the box");
	ExpectColor(At(frame, 32, 18), Background.r, Background.g, Background.b, "outside the box, right");
	EXPECT_EQ(static_cast<uint8_t *>(pal->pixels)[0 * pal->pitch + 9], Own) << "the extension takes the nearest pixel's index";
	EXPECT_EQ(static_cast<uint8_t *>(pal->pixels)[0 * pal->pitch + 8], BackgroundIndex) << "the PalSurface beside the tip is as the game drew it";
	SDL_FreeSurface(frame);
	SDL_FreeSurface(pal);
}

TEST(HdTest, BoxedArtWithoutFreeSilhouetteStaysInsideTheFootprint)
{
	SDL_Surface *frame = DrawBoxScenario("boxbound", "-4 -8 12 14", false, 88);
	ASSERT_NE(frame, nullptr);
	ExpectColor(At(frame, 16, 16), CornerArt.r, CornerArt.g, CornerArt.b, "the picture sits where its box says");
	ExpectColor(At(frame, 23, 23), FrontArt.r, FrontArt.g, FrontArt.b, "the art inside the sprite");
	ExpectColor(At(frame, 18, 14), Background.r, Background.g, Background.b, "no tip outside the footprint");
	ExpectColor(At(frame, 20, 26), Background.r, Background.g, Background.b, "no foot outside the footprint");
	SDL_FreeSurface(frame);
}

TEST(HdTest, BoxedArtGivesWayToLaterDrawsAndToTheSurfacesEdge)
{
	// After the sprite: a draw that doesn't know the HD layer changes lo pixel (9, 6), and a sprite with art covers lo pixels x 8..11, y 0..3
	SDL_Surface *frame = DrawBoxScenario("boxlater", "-4 -8 12 14", true, 92, false, 0, [](SDL_Surface *s, ClxSprite other) {
		static_cast<uint8_t *>(s->pixels)[6 * s->pitch + 9] = Unique;
		ClxDraw(Surface(s), { 8, 3 }, other);
	});
	ASSERT_NE(frame, nullptr);
	const SDL_Color unique = Palette()[Unique];
	ExpectColor(At(frame, 18, 12), unique.r, unique.g, unique.b, "a pixel drawn later without HD art covers the tip");
	ExpectColor(At(frame, 18, 2), OtherArt.r, OtherArt.g, OtherArt.b, "a sprite drawn later covers the tip");
	ExpectColor(At(frame, 18, 10), TipArt.r, TipArt.g, TipArt.b, "the tip between them");
	SDL_FreeSurface(frame);
	// Drawn into the rows from 4 down: the tip stops at the surface's edge
	SDL_Surface *clipped = DrawBoxScenario("boxclip", "-4 -8 12 14", true, 96, false, 4);
	ASSERT_NE(clipped, nullptr);
	ExpectColor(At(clipped, 18, 6), Background.r, Background.g, Background.b, "no tip above the surface the sprite is drawn into");
	ExpectColor(At(clipped, 18, 8), TipArt.r, TipArt.g, TipArt.b, "the tip from its first row");
	ExpectColor(At(clipped, 16, 16), CornerArt.r, CornerArt.g, CornerArt.b, "the sprite");
	SDL_FreeSurface(clipped);
}

TEST(HdTest, EffectLayerOfBoxedArtStaysRelativeToTheSprite)
{
	SDL_Surface *frame = DrawBoxScenario("boxfx", "-4 -8 12 14", true, 100, true);
	ASSERT_NE(frame, nullptr);
	ExpectColor(At(frame, 12, 8), 10, 20, 30, "laid over at the layer's top-left, 2 left of and 4 above the sprite");
	ExpectColor(At(frame, 24, 10), 150, 160, 170, "added right of the sprite");
	ExpectColor(At(frame, 18, 0), TipArt.r, TipArt.g, TipArt.b, "the tip");
	ExpectColor(At(frame, 16, 16), CornerArt.r, CornerArt.g, CornerArt.b, "the sprite");
	SDL_FreeSurface(frame);
}

TEST(HdTest, ABoxThatCantBeReadIsIgnored)
{
	// The picture is then taken to cover the sprite, as any art without a box: nothing is drawn beyond the sprite's rectangle
	SDL_Surface *frame = DrawBoxScenario("boxbad", "-4 -8 0 14", true, 104);
	ASSERT_NE(frame, nullptr);
	ExpectColor(At(frame, 18, 14), Background.r, Background.g, Background.b, "nothing above the sprite");
	ExpectColor(At(frame, 18, 0), Background.r, Background.g, Background.b, "nothing far above the sprite");
	ExpectColor(At(frame, 8, 18), Background.r, Background.g, Background.b, "nothing left of the sprite");
	SDL_FreeSurface(frame);
}

// Shading (hd.hpp, "Shading"): two palette ramps that a light table darkens by different shares, and a recolour
constexpr uint8_t Other = 40;
constexpr uint8_t OwnLit = 50;
constexpr uint8_t OtherLit = 60;
constexpr uint8_t OwnDim = 70;
constexpr uint8_t OtherDim = 80;
constexpr uint8_t Black = 0;

std::array<SDL_Color, 256> ShadingPalette()
{
	std::array<SDL_Color, 256> colors = Palette();
	colors[Black] = { 0, 0, 0, 255 };
	colors[Other] = { 100, 150, 100, 255 };
	colors[OwnLit] = { 100, 25, 25, 255 };    // Own at half light
	colors[OtherLit] = { 25, 37, 25, 255 };   // Other at a quarter
	colors[OwnDim] = { 160, 10, 10, 255 };    // Own and Other 40 darker, as a light table steps a colour down its ramp
	colors[OtherDim] = { 60, 110, 60, 255 };
	return colors;
}

/** A CLX list of one 8x4 sprite: the left half of every row ``Own``, the right half ``Other``. */
std::vector<uint8_t> TwoToneSprite()
{
	std::vector<uint8_t> data = { 1, 0, 0, 0, 12, 0, 0, 0, 54, 0, 0, 0, 6, 0, 8, 0, 4, 0 };
	for (int row = 0; row < 4; ++row)
		data.insert(data.end(), { 0xF8, Own, Own, Own, Own, Other, Other, Other, Other });
	return data;
}

/**
 * Draws the two-tone sprite (lo pixels x 4..11, y 1..4 of a surface 6 high) through ``trn`` (none: ClxDraw), with flat art ``FrontArt``,
 * in a pack with ``extra`` lines; ``lightTable``: ``trn`` is one of the game's light tables (the one of light level ``level``). ``between``
 * runs after the draw, before the frame is presented. Returns the HD frame (scale 2).
 */
SDL_Surface *DrawShadingScenario(const std::string &name, const std::string &extra, const uint8_t *trn, bool lightTable,
    const std::function<void(SDL_Surface *, const Surface &)> &between = {}, int level = 0)
{
	const fs::path pack = fs::temp_directory_path() / ("devilutionx_hd_" + name);
	const fs::path dump = pack / "dump";
	fs::remove_all(pack);
	fs::create_directories(dump);
	const std::array<SDL_Color, 256> colors = ShadingPalette();
	{
		fs::create_directories(pack / "palettes");
		std::ofstream pal(pack / "palettes/test.pal", std::ios::binary);
		for (const SDL_Color &c : colors)
			pal << c.r << c.g << c.b;
		std::ofstream index(pack / "hd.txt");
		index << "scale 2\n" << extra;
		// A monster's art: the game world's (the panels' recoloured art keeps the per-pixel grade)
		fs::create_directories(pack / "sprite/monsters" / name / "two");
		SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, 16, 8, 32, SDL_PIXELFORMAT_ARGB8888);
		SDL_FillRect(s, nullptr, SDL_MapRGBA(s->format, FrontArt.r, FrontArt.g, FrontArt.b, 255));
		IMG_SavePNG(s, (pack / "sprite/monsters" / name / "two/0.0.png").string().c_str());
		SDL_FreeSurface(s);
		index << "image sprite/monsters/" << name << "/two/0.0.png palettes/test.pal\n";
	}
	setenv("DEVILUTIONX_HD_PACK", pack.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP", dump.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP_EVERY", "1", 1);
	hd::Init();
	EXPECT_TRUE(hd::Enabled());
	static std::vector<uint8_t> tables;
	if (lightTable && level > 0) {
		tables.assign(16 * 256, 0);
		std::copy_n(trn, 256, tables.begin() + level * 256);
		trn = tables.data() + level * 256;
		hd::SetLightTables(tables.data(), tables.size());
	} else {
		hd::SetLightTables(lightTable ? trn : nullptr, lightTable ? 256 : 0);
	}

	// Not the size of any other test's surface (nor of this scenario's other calls), so that the provenance plane starts clean
	static int calls = 0;
	SDL_Surface *pal = SDL_CreateRGBSurfaceWithFormat(0, 16 + 4 * calls++, 6, 8, SDL_PIXELFORMAT_INDEX8);
	SDL_SetPaletteColors(pal->format->palette, colors.data(), 0, 256);
	SDL_FillRect(pal, nullptr, BackgroundIndex);
	hd::SetTarget(pal);
	const Surface out(pal);
	static std::vector<uint8_t> two;
	two = TwoToneSprite();
	hd::RegisterSpriteList("Monsters\\" + name + "\\Two.CL2", 0, ClxSpriteList { two.data() });
	if (trn != nullptr)
		ClxDrawTRN(out, { 4, 4 }, ClxSpriteList { two.data() }[0], trn);
	else
		ClxDraw(out, { 4, 4 }, ClxSpriteList { two.data() }[0]);
	if (between) between(pal, out);
	hd::Present(nullptr, pal);
	SDL_Surface *frame = LoadFrame(dump);
	hd::SetLightTables(nullptr, 0);
	fs::remove_all(pack);
	SDL_FreeSurface(pal);
	return frame;
}

std::array<uint8_t, 256> Identity()
{
	std::array<uint8_t, 256> t;
	for (int i = 0; i < 256; ++i)
		t[i] = static_cast<uint8_t>(i);
	return t;
}

/** A channel of ``art`` through the line from (``refA``, ``liveA``) to (``refB``, ``liveB``) (the sprite's two colours), as the layer rounds it. */
int LineGraded(int art, int liveA, int liveB, int refA, int refB)
{
	const double scale = static_cast<double>(liveB - liveA) / (refB - refA);
	const double shift = liveA - scale * refA;
	const int64_t v = ((static_cast<int64_t>(art) * std::lround(scale * 65536)) >> 16) + std::lround(shift);
	return static_cast<int>(std::clamp<int64_t>(v, 0, 255));
}

TEST(HdTest, ALitSpriteIsShadedAsAWholeNotPixelByPixel)
{
	// The light table darkens Own to half and Other to a quarter: per pixel that would draw the original's two halves into the flat art
	std::array<uint8_t, 256> light = Identity();
	light[Own] = OwnLit;
	light[Other] = OtherLit;
	SDL_Surface *frame = DrawShadingScenario("shadewhole", "", light.data(), true);
	ASSERT_NE(frame, nullptr);
	const SDL_Color left = At(frame, 9, 4), right = At(frame, 22, 4);
	ExpectColor(right, left.r, left.g, left.b, "both halves alike");
	ExpectColor(left, LineGraded(FrontArt.r, 100, 25, 200, 100), LineGraded(FrontArt.g, 25, 37, 50, 150), LineGraded(FrontArt.b, 25, 25, 50, 100),
	    "the line through the sprite's (reference, live) colours");
	SDL_FreeSurface(frame);
}

TEST(HdTest, DimLightTakesTheSameBrightnessOffEveryColour)
{
	// A light table that steps both colours 40 down: the art loses 40 too (a ratio would take less off its dark channels, 0.73 x 60 = 44)
	std::array<uint8_t, 256> dim = Identity();
	dim[Own] = OwnDim;
	dim[Other] = OtherDim;
	SDL_Surface *frame = DrawShadingScenario("shadedim", "", dim.data(), true);
	ASSERT_NE(frame, nullptr);
	ExpectColor(At(frame, 9, 4), FrontArt.r - 40, FrontArt.g - 40, FrontArt.b - 40, "40 darker");
	ExpectColor(At(frame, 22, 4), FrontArt.r - 40, FrontArt.g - 40, FrontArt.b - 40, "40 darker, both halves alike");
	SDL_FreeSurface(frame);
}

TEST(HdTest, ASpriteLitToBlackIsBlack)
{
	// A factor of 0 used to mean "add the offset": art lit to black kept most of its colour (monsters showing in the dark)
	std::array<uint8_t, 256> dark {};
	SDL_Surface *frame = DrawShadingScenario("shadeblack", "", dark.data(), true);
	ASSERT_NE(frame, nullptr);
	ExpectColor(At(frame, 9, 4), 0, 0, 0, "lit to black");
	ExpectColor(At(frame, 22, 4), 0, 0, 0, "lit to black");
	SDL_FreeSurface(frame);
}

TEST(HdTest, AMonsterInDimLightKeepsAMinimumBrightnessWhenThePackAsks)
{
	// "minbright a 0.25": no channel of a monster's art is darker than a quarter of its own, so art lit to black keeps a quarter of its colour
	std::array<uint8_t, 256> dark {};
	SDL_Surface *frame = DrawShadingScenario("minbright", "minbright a 0.25\n", dark.data(), true);
	ASSERT_NE(frame, nullptr);
	ExpectColor(At(frame, 9, 4), FrontArt.r / 4, FrontArt.g / 4, FrontArt.b / 4, "a quarter of the art");
	ExpectColor(At(frame, 22, 4), FrontArt.r / 4, FrontArt.g / 4, FrontArt.b / 4, "a quarter of the art, both halves alike");
	SDL_FreeSurface(frame);

	// Art that the light leaves brighter than that is as it was
	std::array<uint8_t, 256> dim = Identity();
	dim[Own] = OwnDim;
	dim[Other] = OtherDim;
	frame = DrawShadingScenario("minbrightdim", "minbright a 0.25\n", dim.data(), true);
	ASSERT_NE(frame, nullptr);
	ExpectColor(At(frame, 9, 4), FrontArt.r - 40, FrontArt.g - 40, FrontArt.b - 40, "40 darker, above the minimum");
	SDL_FreeSurface(frame);
}

TEST(HdTest, TheMinimumBrightnessFadesOutAtTheEdgeOfTheLight)
{
	// Full up to light level 11, then 3/4, 1/2, 1/4, and nothing at 15: what the game draws black at the edge of the light stays black
	std::array<uint8_t, 256> dark {};
	SDL_Surface *frame = DrawShadingScenario("minbrighthalf", "minbright a 0.25\n", dark.data(), true, {}, 13);
	ASSERT_NE(frame, nullptr);
	ExpectColor(At(frame, 9, 4), 15, 7, 7, "half of the minimum at level 13 (0.25 x 127/255 of the art)");
	SDL_FreeSurface(frame);
	frame = DrawShadingScenario("minbrightedge", "minbright a 0.25\n", dark.data(), true, {}, 15);
	ASSERT_NE(frame, nullptr);
	ExpectColor(At(frame, 9, 4), 0, 0, 0, "black at level 15");
	SDL_FreeSurface(frame);
}

TEST(HdTest, TheMinimumBrightnessLeavesARecolouredMonsterAlone)
{
	// A TRN that isn't a light table (Infravision, Stone Curse, a unique's colours) is a recolour, not dim light: the minimum doesn't apply
	std::array<uint8_t, 256> recolour {};
	SDL_Surface *plain = DrawShadingScenario("minbrightrecolourplain", "", recolour.data(), false);
	ASSERT_NE(plain, nullptr);
	const SDL_Color left = At(plain, 9, 4);
	const SDL_Color right = At(plain, 22, 4);
	SDL_FreeSurface(plain);
	SDL_Surface *frame = DrawShadingScenario("minbrightrecolour", "minbright a 0.25\n", recolour.data(), false);
	ASSERT_NE(frame, nullptr);
	ExpectColor(At(frame, 9, 4), left.r, left.g, left.b, "as without the minimum");
	ExpectColor(At(frame, 22, 4), right.r, right.g, right.b, "as without the minimum, both halves alike");
	SDL_FreeSurface(frame);
}

TEST(HdTest, ARecolourKeepsItsRegions)
{
	// Through a TRN that isn't a light table only Own changes: the left edge takes the recolour, the right edge keeps its colour
	std::array<uint8_t, 256> recolour = Identity();
	recolour[Own] = Swapped;
	SDL_Surface *frame = DrawShadingScenario("shaderecolour", "", recolour.data(), false);
	ASSERT_NE(frame, nullptr);
	ExpectColor(At(frame, 8, 4), 30, 60, 240, "left edge: Own, recoloured to Swapped");
	ExpectColor(At(frame, 23, 4), FrontArt.r, FrontArt.g, FrontArt.b, "right edge: Other, as it is");
	SDL_FreeSurface(frame);
}

TEST(HdTest, PixelShadingKeepsTheOldGrade)
{
	std::array<uint8_t, 256> light = Identity();
	light[Own] = OwnLit;
	light[Other] = OtherLit;
	SDL_Surface *frame = DrawShadingScenario("shadepixel", "shading pixel\n", light.data(), true);
	ASSERT_NE(frame, nullptr);
	ExpectColor(At(frame, 9, 4), FrontArt.r / 2, FrontArt.g / 2, FrontArt.b / 2, "Own: half");
	ExpectColor(At(frame, 22, 4), FrontArt.r / 4, FrontArt.g * 37 / 150, FrontArt.b / 4, "Other: a quarter");
	SDL_FreeSurface(frame);
}

TEST(HdTest, ColoursTheLevelAnimatesAreGradedOnTheirOwn)
{
	// Hell's light tables light its blood walls apart from the ramps and rotate them (colour cycling): Own (index 10) is drawn as OwnLit, Other
	// as itself. Without the cycled colours both halves take one grade that fits neither; with them Other keeps its art and Own alone follows
	// the animation, pixel by pixel
	std::array<uint8_t, 256> light = Identity();
	light[Own] = OwnLit;
	SDL_Surface *whole = DrawShadingScenario("cyclednone", "", light.data(), true);
	ASSERT_NE(whole, nullptr);
	const SDL_Color skewed = At(whole, 22, 4);
	EXPECT_FALSE(skewed.r == FrontArt.r && skewed.g == FrontArt.g && skewed.b == FrontArt.b) << "the animated half pulls the other off";
	SDL_FreeSurface(whole);

	hd::SetCycledColors(1, 31);
	SDL_Surface *frame = DrawShadingScenario("cycled", "", light.data(), true);
	hd::SetCycledColors(1, 0);
	ASSERT_NE(frame, nullptr);
	ExpectColor(At(frame, 22, 4), FrontArt.r, FrontArt.g, FrontArt.b, "Other: lit by nothing, as it is");
	ExpectColor(At(frame, 9, 4), FrontArt.r / 2, FrontArt.g / 2, FrontArt.b / 2, "Own: times OwnLit / Own (a half), per pixel");
	SDL_FreeSurface(frame);
}

TEST(HdTest, TheViewForgetsArtDrawnInEarlierFrames)
{
	// The game fills what it doesn't draw (unlit tiles) without the layer seeing it; a pixel that keeps its index must not keep its art
	SDL_Surface *frame = DrawShadingScenario("shadeforget", "", nullptr, false, [](SDL_Surface *, const Surface &out) { hd::ForgetView(out); });
	ASSERT_NE(frame, nullptr);
	const SDL_Color own = ShadingPalette()[Own];
	ExpectColor(At(frame, 9, 4), own.r, own.g, own.b, "the pixel's own colour, no art");
	SDL_FreeSurface(frame);
}

TEST(HdTest, DroppedItemsDrawTheArtOfTheirOwnItemGraphic)
{
	const fs::path pack = fs::temp_directory_path() / "devilutionx_hd_items";
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
		WriteArt(pack, "sprite/items/swrdflip/0.0.png", { 120, 60, 60 }, index);      // what every sword's drop shares
		WriteArt(pack, "sprite/items/swrdflip@c057/0.0.png", { 60, 120, 60 }, index); // the Bastard Sword's own
		WriteArt(pack, "sprite/items/axe@c144/0.0.png", { 60, 60, 120 }, index);      // an item graphic's own, no shared art
	}
	setenv("DEVILUTIONX_HD_PACK", pack.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP", dump.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP_EVERY", "1", 1);
	hd::Init();
	ASSERT_TRUE(hd::Enabled());

	SDL_Surface *pal = SDL_CreateRGBSurfaceWithFormat(0, 48, 8, 8, SDL_PIXELFORMAT_INDEX8);
	SDL_SetPaletteColors(pal->format->palette, colors.data(), 0, 256);
	SDL_FillRect(pal, nullptr, 0);
	hd::SetTarget(pal);
	const Surface out(pal);

	std::vector<uint8_t> sword = Sprite();
	std::vector<uint8_t> axe = Sprite();
	hd::RegisterSpriteList("Items\\Swrdflip.CEL", 0, ClxSpriteList { sword.data() });
	hd::RegisterSpriteList("Items\\Axe.CEL", 0, ClxSpriteList { axe.data() });
	ClxDraw(out, { 0, 3 }, ClxSpriteList { sword.data() }[0]); // no item named: the shared art
	{
		const hd::ItemGraphic bastardSword(57);
		ClxDraw(out, { 8, 3 }, ClxSpriteList { sword.data() }[0]);
	}
	{
		const hd::ItemGraphic shortSword(64); // no art of its own: the shared art
		ClxDraw(out, { 16, 3 }, ClxSpriteList { sword.data() }[0]);
	}
	{
		const hd::ItemGraphic axeGraphic(144);
		ClxDraw(out, { 24, 3 }, ClxSpriteList { axe.data() }[0]);
	}
	{
		const hd::ItemGraphic battleAxe(101); // neither: the original sprite
		ClxDraw(out, { 32, 3 }, ClxSpriteList { axe.data() }[0]);
	}
	ClxDraw(out, { 40, 3 }, ClxSpriteList { sword.data() }[0]); // the item's scope has ended
	hd::Present(nullptr, pal);

	SDL_Surface *argb = LoadFrame(dump);
	ASSERT_NE(argb, nullptr);
	ExpectColor(At(argb, 2, 2), 120, 60, 60, "a sword drawn without an item graphic");
	ExpectColor(At(argb, 18, 2), 60, 120, 60, "the Bastard Sword: its own art");
	ExpectColor(At(argb, 34, 2), 120, 60, 60, "the Short Sword: the art swords share");
	ExpectColor(At(argb, 50, 2), 60, 60, 120, "an axe graphic with art of its own");
	ExpectColor(At(argb, 66, 2), 200, 50, 50, "an axe graphic without: the original pixels");
	ExpectColor(At(argb, 82, 2), 120, 60, 60, "after the item's draw");
	SDL_FreeSurface(argb);
	SDL_FreeSurface(pal);
	fs::remove_all(pack);
}

TEST(HdTest, AUniqueItemCanHaveArtOfItsOwn)
{
	const fs::path pack = fs::temp_directory_path() / "devilutionx_hd_unique_items";
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
		WriteArt(pack, "sprite/items/uniqflip@c057/0.0.png", { 60, 120, 60 }, index);                               // the Bastard Sword's
		WriteArt(pack, "sprite/items/uniqflip@c057~the-butcher-s-cleaver/0.0.png", { 60, 60, 120 }, index);        // one unique's with that picture
		WriteArt(pack, "sprite/data/inv/objcurs@c057~the-butcher-s-cleaver/0.0.png", { 120, 120, 60 }, index);     // the same unique's inventory picture
	}
	setenv("DEVILUTIONX_HD_PACK", pack.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP", dump.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP_EVERY", "1", 1);
	hd::Init();
	ASSERT_TRUE(hd::Enabled());

	SDL_Surface *pal = SDL_CreateRGBSurfaceWithFormat(0, 64, 8, 8, SDL_PIXELFORMAT_INDEX8);
	SDL_SetPaletteColors(pal->format->palette, colors.data(), 0, 256);
	SDL_FillRect(pal, nullptr, 0);
	hd::SetTarget(pal);
	const Surface out(pal);

	std::vector<uint8_t> sword = Sprite();
	std::vector<uint8_t> picture = Sprite();
	hd::RegisterSpriteList("Items\\Uniqflip.CEL", 0, ClxSpriteList { sword.data() });
	hd::RegisterSpriteList("Data\\Inv\\Objcurs.CEL", 0, ClxSpriteList { picture.data() });
	const std::string_view cleaver = "The Butcher's Cleaver";
	{
		const hd::ItemGraphic scope(57, true, 5, cleaver); // the unique on the floor
		ClxDraw(out, { 0, 3 }, ClxSpriteList { sword.data() }[0]);
	}
	{
		const hd::ItemGraphic scope(57, true, 6, "A Unique Without Art"); // another unique with that picture: the picture's art
		ClxDraw(out, { 8, 3 }, ClxSpriteList { sword.data() }[0]);
	}
	{
		const hd::ItemGraphic scope(57, true, -1, {}); // a plain item: the picture's art
		ClxDraw(out, { 16, 3 }, ClxSpriteList { sword.data() }[0]);
	}
	{
		const hd::ItemGraphic scope(58, true, 5, cleaver); // the table entry changed under the same index: no art of its own
		ClxDraw(out, { 24, 3 }, ClxSpriteList { sword.data() }[0]);
	}
	{
		const hd::ItemGraphic scope(57, false, 5, cleaver); // the unique's inventory picture
		ClxDraw(out, { 32, 3 }, ClxSpriteList { picture.data() }[0]);
	}
	{
		const hd::ItemGraphic scope(57, false, -1, {}); // a plain item's: no art for the picture here, the original pixels
		ClxDraw(out, { 40, 3 }, ClxSpriteList { picture.data() }[0]);
	}
	{
		const hd::ItemGraphic scope(57, true, 5, cleaver); // drawn again: what was looked up before
		ClxDraw(out, { 48, 3 }, ClxSpriteList { sword.data() }[0]);
	}
	hd::Present(nullptr, pal);

	SDL_Surface *argb = LoadFrame(dump);
	ASSERT_NE(argb, nullptr);
	ExpectColor(At(argb, 2, 2), 60, 60, 120, "the unique on the floor: its own art");
	ExpectColor(At(argb, 18, 2), 60, 120, 60, "another unique: its picture's art");
	ExpectColor(At(argb, 34, 2), 60, 120, 60, "a plain item: its picture's art");
	ExpectColor(At(argb, 50, 2), 200, 50, 50, "another picture: the original pixels");
	ExpectColor(At(argb, 66, 2), 120, 120, 60, "the unique in the inventory: its own art");
	ExpectColor(At(argb, 82, 2), 200, 50, 50, "a plain item in the inventory: the original pixels");
	ExpectColor(At(argb, 98, 2), 60, 60, 120, "the unique again");
	SDL_FreeSurface(argb);
	SDL_FreeSurface(pal);
	fs::remove_all(pack);
}

/** An owned copy of a CLX list, as the game loads them: freeing it tells the HD layer (ClxFreeHook). */
OwnedClxSpriteList Owned(const std::vector<uint8_t> &data)
{
	std::unique_ptr<uint8_t[]> copy { new uint8_t[data.size()] };
	std::copy(data.begin(), data.end(), copy.get());
	return OwnedClxSpriteList { std::move(copy) };
}

/** The pixels of the dumped HD frame ``name``. */
std::vector<uint32_t> DumpedFrame(const fs::path &dump, const std::string &name)
{
	SDL_Surface *loaded = IMG_LoadPNG((dump / ("hd-" + name + ".png")).string().c_str());
	if (loaded == nullptr) return {};
	SDL_Surface *argb = SDL_ConvertSurfaceFormat(loaded, SDL_PIXELFORMAT_ARGB8888, 0);
	SDL_FreeSurface(loaded);
	std::vector<uint32_t> pixels;
	for (int y = 0; y < argb->h; ++y) {
		const auto *row = reinterpret_cast<const uint32_t *>(static_cast<const uint8_t *>(argb->pixels) + static_cast<size_t>(y) * argb->pitch);
		pixels.insert(pixels.end(), row, row + argb->w);
	}
	SDL_FreeSurface(argb);
	return pixels;
}

/** A pack of one sprite's art (``file``: 8 x 8 of 64 colours, so that a frame drawn from it shows whether every pixel came back the same). */
struct MemoryPack {
	fs::path pack;
	fs::path dump;
	SDL_Surface *pal = nullptr;

	MemoryPack(const std::string &name, const std::string &file, const char *budgetMb = nullptr)
	{
		pack = fs::temp_directory_path() / ("devilutionx_hd_" + name);
		dump = pack / "dump";
		fs::remove_all(pack);
		fs::create_directories(dump);
		fs::create_directories(pack / "palettes");
		const std::array<SDL_Color, 256> colors = Palette();
		{
			std::ofstream palFile(pack / "palettes/test.pal", std::ios::binary);
			for (const SDL_Color &c : colors)
				palFile << c.r << c.g << c.b;
			std::ofstream index(pack / "hd.txt");
			index << "scale 2\n";
			fs::create_directories((pack / file).parent_path());
			SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, 8, 8, 32, SDL_PIXELFORMAT_ARGB8888);
			for (int y = 0; y < 8; ++y)
				for (int x = 0; x < 8; ++x)
					static_cast<uint32_t *>(s->pixels)[y * (s->pitch / 4) + x] = SDL_MapRGBA(s->format, 40 + 20 * x, 40 + 20 * y, 90, 255);
			IMG_SavePNG(s, (pack / file).string().c_str());
			SDL_FreeSurface(s);
			index << "image " << file << " palettes/test.pal\n";
		}
		setenv("DEVILUTIONX_HD_PACK", pack.string().c_str(), 1);
		setenv("DEVILUTIONX_HD_DUMP", dump.string().c_str(), 1);
		setenv("DEVILUTIONX_HD_DUMP_EVERY", "0", 1);
		if (budgetMb != nullptr)
			setenv("DEVILUTIONX_HD_MEMORY_MB", budgetMb, 1);
		else
			unsetenv("DEVILUTIONX_HD_MEMORY_MB");
		hd::Init();
		pal = SDL_CreateRGBSurfaceWithFormat(0, 16, 8, 8, SDL_PIXELFORMAT_INDEX8);
		SDL_SetPaletteColors(pal->format->palette, colors.data(), 0, 256);
		SDL_FillRect(pal, nullptr, 0);
		hd::SetTarget(pal);
	}

	~MemoryPack()
	{
		unsetenv("DEVILUTIONX_HD_MEMORY_MB");
		SDL_FreeSurface(pal);
		fs::remove_all(pack);
	}

	/** Frames of the game view drawn anew with nothing in it (the hero's gone elsewhere), ``n`` times. */
	void EmptyFrames(int n) const
	{
		for (int i = 0; i < n; ++i) {
			hd::ForgetView(Surface(pal));
			hd::Present(nullptr, pal);
		}
	}

	/** A frame of the game view showing ``sprites`` side by side, dumped as ``name``. */
	std::vector<uint32_t> Frame(const std::string &name, const std::vector<ClxSprite> &sprites) const
	{
		const Surface out(pal);
		hd::ForgetView(out);
		for (size_t i = 0; i < sprites.size(); ++i)
			ClxDraw(out, { static_cast<int>(i) * 8, 3 }, sprites[i]);
		hd::RequestDump(name);
		hd::Present(nullptr, pal);
		return DumpedFrame(dump, name);
	}
};

TEST(HdTest, ArtIsLetGoOfWhenTheGameFreesItsSpritesAndDrawsTheSameWhenLoadedAgain)
{
	const MemoryPack pack("unload", "sprite/unload/a/0.0.png");
	ASSERT_TRUE(hd::Enabled());
	const hd::MemoryUse before = hd::Memory();

	// Two heroes in the same gear: two copies of one file, one image
	std::optional<OwnedClxSpriteList> first = Owned(Sprite());
	std::optional<OwnedClxSpriteList> second = Owned(Sprite());
	hd::RegisterSpriteList("Unload\\A.CL2", 0, *first);
	hd::RegisterSpriteList("Unload\\A.CL2", 0, *second);
	EXPECT_FALSE(hd::Loaded("sprite/unload/a/0.0.png")) << "loaded when first drawn";
	const std::vector<uint32_t> drawn = pack.Frame("drawn", { (*first)[0], (*second)[0] });
	ASSERT_FALSE(drawn.empty());
	EXPECT_TRUE(hd::Loaded("sprite/unload/a/0.0.png"));
	EXPECT_EQ(hd::Memory().loads, before.loads + 1);

	first.reset(); // one hero changes gear: the other still draws the image
	pack.EmptyFrames(20);
	EXPECT_TRUE(hd::Loaded("sprite/unload/a/0.0.png"));

	second.reset(); // nothing draws it any more
	pack.EmptyFrames(20);
	EXPECT_FALSE(hd::Loaded("sprite/unload/a/0.0.png"));

	// Loaded again (perhaps at the same address), it draws exactly as before
	const OwnedClxSpriteList again = Owned(Sprite());
	hd::RegisterSpriteList("Unload\\A.CL2", 0, again);
	const std::vector<uint32_t> redrawn = pack.Frame("redrawn", { again[0], again[0] });
	EXPECT_TRUE(hd::Loaded("sprite/unload/a/0.0.png"));
	EXPECT_EQ(hd::Memory().loads, before.loads + 2);
	EXPECT_TRUE(redrawn == drawn);
}

TEST(HdTest, ArtStaysWhileTheSameSpritesAreLoadedAgainRightAway)
{
	const MemoryPack pack("unload_grace", "sprite/unloadgrace/a/0.0.png");
	const hd::MemoryUse before = hd::Memory();
	std::optional<OwnedClxSpriteList> sprites = Owned(Sprite());
	hd::RegisterSpriteList("UnloadGrace\\A.CL2", 0, *sprites);
	const std::vector<uint32_t> drawn = pack.Frame("drawn", { (*sprites)[0] });
	// A level change: the hero's graphics are freed, the loading screen shows for a while, and the same ones are loaded for the new level
	sprites.reset();
	for (int i = 0; i < 100; ++i)
		hd::Present(nullptr, pack.pal);
	sprites = Owned(Sprite());
	hd::RegisterSpriteList("UnloadGrace\\A.CL2", 0, *sprites);
	const std::vector<uint32_t> redrawn = pack.Frame("redrawn", { (*sprites)[0] });
	pack.EmptyFrames(1);
	EXPECT_EQ(hd::Memory().loads, before.loads + 1) << "drawn from the image it had";
	EXPECT_TRUE(hd::Loaded("sprite/unloadgrace/a/0.0.png"));
	EXPECT_TRUE(redrawn == drawn);
}

TEST(HdTest, TheBudgetLetsGoOfIdleArtThatIsLoadedAgainWhenDrawn)
{
	const MemoryPack pack("unload_budget", "sprite/unloadbudget/a/0.0.png", "0.0001"); // about 100 bytes: less than the image
	const hd::MemoryUse before = hd::Memory();
	const OwnedClxSpriteList sprites = Owned(Sprite());
	hd::RegisterSpriteList("UnloadBudget\\A.CL2", 0, sprites);
	const std::vector<uint32_t> drawn = pack.Frame("drawn", { sprites[0] });
	EXPECT_EQ(hd::Memory().loads, before.loads + 1);
	pack.EmptyFrames(100); // not idle for long enough
	const std::vector<uint32_t> soon = pack.Frame("soon", { sprites[0] });
	EXPECT_EQ(hd::Memory().loads, before.loads + 1) << "still loaded";
	pack.EmptyFrames(700); // idle, and the budget is exceeded: let go of, though its sprite is still registered
	EXPECT_FALSE(hd::Loaded("sprite/unloadbudget/a/0.0.png"));
	const std::vector<uint32_t> redrawn = pack.Frame("redrawn", { sprites[0] });
	EXPECT_EQ(hd::Memory().loads, before.loads + 2) << "loaded again";
	EXPECT_TRUE(soon == drawn);
	EXPECT_TRUE(redrawn == drawn);
}

/**
 * Draws art 16 pixels wide and ``rows`` high (at scale 4 over a 4 x 4 sprite: one art pixel per HD pixel across, rows sampled down to 16),
 * every pixel opaque and of a colour of its own but the last, which is fully transparent with a colour of its own, through no palette (as
 * it is). Returns the HD frame's 16 x 16 block, what it should show, and how the art was kept (``indexed``).
 */
void DrawManyColours(const std::string &name, int rows, std::vector<uint32_t> &shown, std::vector<uint32_t> &expected, bool &indexed)
{
	const fs::path pack = fs::temp_directory_path() / ("devilutionx_hd_" + name);
	const fs::path dump = pack / "dump";
	fs::remove_all(pack);
	fs::create_directories(dump);
	const std::string file = "sprite/" + name + "/a/0.0.png";
	std::vector<uint32_t> art(static_cast<size_t>(16) * rows);
	for (size_t i = 0; i < art.size(); ++i)
		art[i] = 0xFF000000U | static_cast<uint32_t>((i * 37) & 0xFF) << 16 | static_cast<uint32_t>(i & 0xFF) << 8 | static_cast<uint32_t>(i >> 8);
	art.back() = 0x0012AB34U; // transparent: its colour is never seen
	{
		std::ofstream index(pack / "hd.txt");
		index << "scale 4\nimage " << file << " -\n";
		fs::create_directories((pack / file).parent_path());
		SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, 16, rows, 32, SDL_PIXELFORMAT_ARGB8888);
		for (int y = 0; y < rows; ++y)
			std::copy_n(&art[static_cast<size_t>(y) * 16], 16, reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(s->pixels) + static_cast<size_t>(y) * s->pitch));
		IMG_SavePNG(s, (pack / file).string().c_str());
		SDL_FreeSurface(s);
	}
	expected.clear();
	for (int y = 0; y < 16; ++y)
		for (int x = 0; x < 16; ++x) {
			const uint32_t c = art[static_cast<size_t>(y * rows / 16) * 16 + x];
			expected.push_back((c >> 24) == 0 ? 0xFFC83232U : c); // a transparent pixel shows the sprite's own colour
		}
	setenv("DEVILUTIONX_HD_PACK", pack.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP", dump.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP_EVERY", "0", 1);
	hd::Init();
	const std::array<SDL_Color, 256> colors = Palette();
	SDL_Surface *pal = SDL_CreateRGBSurfaceWithFormat(0, 8, 8, 8, SDL_PIXELFORMAT_INDEX8);
	SDL_SetPaletteColors(pal->format->palette, colors.data(), 0, 256);
	SDL_FillRect(pal, nullptr, 0);
	hd::SetTarget(pal);
	const Surface out(pal);
	hd::ForgetView(out);
	const OwnedClxSpriteList sprite = Owned(Sprite());
	hd::RegisterSpriteList(name + "\\A.CL2", 0, sprite);
	ClxDraw(out, { 0, 3 }, sprite[0]);
	hd::RequestDump("many");
	hd::Present(nullptr, pal);
	indexed = hd::Memory(file).indexed != 0;
	const std::vector<uint32_t> frame = DumpedFrame(dump, "many"); // 32 x 32
	shown.clear();
	for (int y = 0; y < 16 && !frame.empty(); ++y)
		shown.insert(shown.end(), &frame[static_cast<size_t>(y) * 32], &frame[static_cast<size_t>(y) * 32] + 16);
	SDL_FreeSurface(pal);
	fs::remove_all(pack);
}

TEST(HdTest, ArtOf256ColoursIsKeptAs8BitAndDrawsExactly)
{
	std::vector<uint32_t> shown, expected;
	bool indexed = false;
	DrawManyColours("colours256", 16, shown, expected, indexed); // 255 opaque colours and a transparent one
	EXPECT_TRUE(indexed);
	EXPECT_TRUE(shown == expected);
}

TEST(HdTest, ArtOfMoreColoursIsKeptInFullColourAndDrawsExactly)
{
	std::vector<uint32_t> shown, expected;
	bool indexed = true;
	DrawManyColours("colours272", 17, shown, expected, indexed); // 271 opaque colours and a transparent one
	EXPECT_FALSE(indexed);
	EXPECT_TRUE(shown == expected);
}


/** One picture, 8 x 8 of one colour, that hd.txt doesn't list (held art is found through its folder's render.json). */
void WritePicture(const fs::path &file, SDL_Color c)
{
	fs::create_directories(file.parent_path());
	SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, 8, 8, 32, SDL_PIXELFORMAT_ARGB8888);
	SDL_FillRect(s, nullptr, SDL_MapRGBA(s->format, c.r, c.g, c.b, 255));
	IMG_SavePNG(s, file.string().c_str());
	SDL_FreeSurface(s);
}

/** A held folder's render.json as the renderer writes it: what it has (``sprites``: path -> frames, every direction) and much else. */
void WriteHeldJson(const fs::path &folder, const std::vector<std::pair<std::string, int>> &sprites)
{
	fs::create_directories(folder);
	std::ofstream json(folder / "render.json");
	json << "{\n \"key\": \"" << folder.filename().string() << "\",\n \"names\": [\"A \\\"name\\\", quoted\"],\n \"set\": \"s\",\n"
	     << " \"per_kit\": {\"plrgfx/warrior/wls\": {\"seat\": {\"docs\": {\"wlsas\": {\"dirs\": [9], \"frames\": -1, \"roll_deg\": [45.0, -1.5e1]}}}, \"on\": true, \"off\": null}},\n"
	     << " \"sprites\": {";
	for (size_t i = 0; i < sprites.size(); ++i)
		json << (i > 0 ? "," : "") << "\n  \"" << sprites[i].first << "\": {\"dirs\": [0, 1, 2, 3, 4, 5, 6, 7], \"frames\": " << sprites[i].second << "}";
	json << "\n },\n \"rendered\": \"2026-11-21T00:00:00-07:00\"\n}\n";
}

TEST(HdTest, HeroesDrawTheArtOfTheWeaponTheyHold)
{
	const fs::path pack = fs::temp_directory_path() / "devilutionx_hd_held";
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
		index << "scale 2\nheld held\n";
		WriteArt(pack, "sprite/plrgfx/warrior/wls/wlsas/0.0.png", { 120, 60, 60 }, index);     // what every sword shares
		WriteArt(pack, "sprite/monsters/zombie/zombiea/0.0.png", { 90, 90, 90 }, index);
	}
	WriteHeldJson(pack / "held/c057", { { "plrgfx/warrior/wls/wlsas", 1 }, { "plrgfx/rogue/rls/rlsas", 1 } });
	WritePicture(pack / "held/c057/plrgfx/warrior/wls/wlsas/0.0.png", { 60, 120, 60 });         // the Bastard Sword's
	WritePicture(pack / "held/c057/plrgfx/rogue/rls/rlsas/0.0.png", { 60, 60, 120 });           // a rogue's, whose set has no art
	WriteHeldJson(pack / "held/c057~doombringer", { { "plrgfx/warrior/wls/wlsas", 1 } });
	WritePicture(pack / "held/c057~doombringer/plrgfx/warrior/wls/wlsas/0.0.png", { 200, 200, 60 }); // a unique's own
	WritePicture(pack / "held/c058/plrgfx/warrior/wls/wlsas/0.0.png", { 30, 30, 30 });          // unfinished: no render.json yet
	unsetenv("DEVILUTIONX_HD_HELD");
	setenv("DEVILUTIONX_HD_PACK", pack.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP", dump.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP_EVERY", "1", 1);
	hd::Init();
	ASSERT_TRUE(hd::Enabled());
	EXPECT_EQ(hd::Held().items, 2U);
	EXPECT_EQ(hd::Held().kits, 3U);

	SDL_Surface *pal = SDL_CreateRGBSurfaceWithFormat(0, 88, 8, 8, SDL_PIXELFORMAT_INDEX8);
	SDL_SetPaletteColors(pal->format->palette, colors.data(), 0, 256);
	SDL_FillRect(pal, nullptr, 0);
	hd::SetTarget(pal);
	const Surface out(pal);

	// Every hero has its own copy of its sprite files, as in the game
	std::vector<std::vector<uint8_t>> heroes(11, Sprite());
	for (size_t i = 0; i < heroes.size(); ++i) {
		const char *file = i == 5 || i == 6 ? "PlrGFX\\Rogue\\RLS\\RLSAS.CL2" : i == 7 ? "Monsters\\Zombie\\ZombieA.CL2" : "PlrGFX\\Warrior\\WLS\\WLSAS.CL2";
		hd::RegisterSpriteList(file, 0, ClxSpriteList { heroes[i].data() });
	}
	const auto draw = [&](size_t i) { ClxDraw(out, { static_cast<int>(i) * 8, 3 }, ClxSpriteList { heroes[i].data() }[0]); };
	draw(0); // no held item named: the set's art
	{
		const hd::HeldItem bastardSword(&heroes[1], 57);
		draw(1);
	}
	{
		const hd::HeldItem doombringer(&heroes[2], 57, 3, "Doombringer");
		draw(2);
	}
	{
		const hd::HeldItem unfinished(&heroes[3], 58);
		draw(3);
	}
	{
		const hd::HeldItem shortSword(&heroes[4], 64); // no art of its own
		draw(4);
	}
	{
		const hd::HeldItem rogue(&heroes[5], 57);
		draw(5);
	}
	draw(6); // the rogue's set has no art: the original sprite
	{
		const hd::HeldItem notAHero(&heroes[7], 57); // a monster drawn while the scope lives keeps its own art
		draw(7);
	}
	{
		const hd::HeldItem shield(&heroes[8], -1); // a shield in hand: the set's art
		draw(8);
	}
	{
		const hd::HeldItem otherUnique(&heroes[9], 57, 5, "The Grandfather"); // a unique without art of its own: its picture's
		draw(9);
	}
	draw(10); // after the scopes: the set's art
	hd::Present(nullptr, pal);
	EXPECT_EQ(hd::HeldArt(&heroes[1]), "c057");
	EXPECT_EQ(hd::HeldArt(&heroes[2]), "c057~doombringer");
	EXPECT_EQ(hd::HeldArt(&heroes[3]), "");
	EXPECT_EQ(hd::HeldArt(&heroes[4]), "");
	EXPECT_EQ(hd::HeldArt(&heroes[8]), "");
	EXPECT_EQ(hd::HeldArt(&heroes[9]), "c057");
	EXPECT_EQ(hd::HeldArt(&heroes[0]), "") << "never noted";

	SDL_Surface *argb = LoadFrame(dump);
	ASSERT_NE(argb, nullptr);
	ExpectColor(At(argb, 2, 2), 120, 60, 60, "a hero drawn without a held item");
	ExpectColor(At(argb, 18, 2), 60, 120, 60, "holding the Bastard Sword: its art");
	ExpectColor(At(argb, 34, 2), 200, 200, 60, "holding Doombringer: the unique's own art");
	ExpectColor(At(argb, 50, 2), 120, 60, 60, "an item whose folder is unfinished: the set's art");
	ExpectColor(At(argb, 66, 2), 120, 60, 60, "an item without held art: the set's art");
	ExpectColor(At(argb, 82, 2), 60, 60, 120, "a rogue holding the Bastard Sword, though the rogue's set has no art");
	ExpectColor(At(argb, 98, 2), 200, 50, 50, "the rogue without a held item: the original pixels");
	ExpectColor(At(argb, 114, 2), 90, 90, 90, "a monster: its own art");
	ExpectColor(At(argb, 130, 2), 120, 60, 60, "a shield in hand: the set's art");
	ExpectColor(At(argb, 146, 2), 60, 120, 60, "a unique without art of its own: its picture's");
	ExpectColor(At(argb, 162, 2), 120, 60, 60, "after the hero's draw");
	SDL_FreeSurface(argb);
	SDL_FreeSurface(pal);
	fs::remove_all(pack);
}

TEST(HdTest, WithoutAHeldFolderHeroesDrawAsBefore)
{
	const fs::path pack = fs::temp_directory_path() / "devilutionx_hd_held_none";
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
		index << "scale 2\n"; // no "held" line: the folder below is not read
		WriteArt(pack, "sprite/plrgfx/warrior/wls/wlsat/0.0.png", { 120, 60, 60 }, index);
	}
	WriteHeldJson(pack / "held/c057", { { "plrgfx/warrior/wls/wlsat", 1 } });
	WritePicture(pack / "held/c057/plrgfx/warrior/wls/wlsat/0.0.png", { 60, 120, 60 });
	unsetenv("DEVILUTIONX_HD_HELD");
	setenv("DEVILUTIONX_HD_PACK", pack.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP", dump.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP_EVERY", "1", 1);
	hd::Init();
	EXPECT_EQ(hd::Held().items, 0U);
	SDL_Surface *pal = SDL_CreateRGBSurfaceWithFormat(0, 8, 8, 8, SDL_PIXELFORMAT_INDEX8);
	SDL_SetPaletteColors(pal->format->palette, colors.data(), 0, 256);
	SDL_FillRect(pal, nullptr, 0);
	hd::SetTarget(pal);
	std::vector<uint8_t> hero = Sprite();
	hd::RegisterSpriteList("PlrGFX\\Warrior\\WLS\\WLSAT.CL2", 0, ClxSpriteList { hero.data() });
	{
		const hd::HeldItem bastardSword(hero.data(), 57);
		ClxDraw(Surface(pal), { 0, 3 }, ClxSpriteList { hero.data() }[0]);
	}
	hd::Present(nullptr, pal);
	SDL_Surface *argb = LoadFrame(dump);
	ASSERT_NE(argb, nullptr);
	ExpectColor(At(argb, 2, 2), 120, 60, 60, "the set's art");
	SDL_FreeSurface(argb);
	SDL_FreeSurface(pal);
	fs::remove_all(pack);
}

/** A pack with a weapon set's stand and attack art and two items' held art for both (64 colours each, so frames compare exactly). */
struct HeldPack {
	fs::path pack;
	fs::path dump;
	SDL_Surface *pal = nullptr;

	static void Write(const fs::path &file, int shade)
	{
		fs::create_directories(file.parent_path());
		SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, 8, 8, 32, SDL_PIXELFORMAT_ARGB8888);
		for (int y = 0; y < 8; ++y)
			for (int x = 0; x < 8; ++x)
				static_cast<uint32_t *>(s->pixels)[y * (s->pitch / 4) + x] = SDL_MapRGBA(s->format, 40 + 20 * x, 40 + 20 * y, shade, 255);
		IMG_SavePNG(s, file.string().c_str());
		SDL_FreeSurface(s);
	}

	explicit HeldPack(const std::string &name)
	{
		pack = fs::temp_directory_path() / ("devilutionx_hd_" + name);
		dump = pack / "dump";
		fs::remove_all(pack);
		fs::create_directories(dump);
		fs::create_directories(pack / "palettes");
		const std::array<SDL_Color, 256> colors = Palette();
		{
			std::ofstream palFile(pack / "palettes/test.pal", std::ios::binary);
			for (const SDL_Color &c : colors)
				palFile << c.r << c.g << c.b;
			std::ofstream index(pack / "hd.txt");
			index << "scale 2\n";
			for (const char *anim : { "wlswl", "wlsht" }) {
				const std::string file = std::string("sprite/plrgfx/warrior/wls/") + anim + "/0.0.png";
				Write(pack / file, 10);
				index << "image " << file << " palettes/test.pal\n";
			}
		}
		for (const char *key : { "c060", "c061" }) { // held art elsewhere, as the studio's preview/held is (DEVILUTIONX_HD_HELD)
			WriteHeldJson(pack / "elsewhere" / key, { { "plrgfx/warrior/wls/wlswl", 1 }, { "plrgfx/warrior/wls/wlsht", 1 } });
			for (const char *anim : { "wlswl", "wlsht" })
				Write(pack / "elsewhere" / key / "plrgfx/warrior/wls" / anim / "0.0.png", key[3] == '0' ? 150 : 250);
		}
		setenv("DEVILUTIONX_HD_PACK", pack.string().c_str(), 1);
		setenv("DEVILUTIONX_HD_HELD", (pack / "elsewhere").string().c_str(), 1);
		setenv("DEVILUTIONX_HD_DUMP", dump.string().c_str(), 1);
		setenv("DEVILUTIONX_HD_DUMP_EVERY", "0", 1);
		unsetenv("DEVILUTIONX_HD_MEMORY_MB");
		hd::Init();
		pal = SDL_CreateRGBSurfaceWithFormat(0, 16, 8, 8, SDL_PIXELFORMAT_INDEX8);
		SDL_SetPaletteColors(pal->format->palette, colors.data(), 0, 256);
		SDL_FillRect(pal, nullptr, 0);
		hd::SetTarget(pal);
	}

	~HeldPack()
	{
		unsetenv("DEVILUTIONX_HD_HELD");
		SDL_FreeSurface(pal);
		fs::remove_all(pack);
	}

	static std::string Held(const char *key, const char *anim)
	{
		return std::string("held/") + key + "/plrgfx/warrior/wls/" + anim + "/0.0.png";
	}

	void EmptyFrames(int n) const
	{
		for (int i = 0; i < n; ++i) {
			hd::ForgetView(Surface(pal));
			hd::Present(nullptr, pal);
		}
	}

	/** A frame of the game view showing the hero ``who`` holding the item graphic ``item``: ``sprites`` side by side. */
	std::vector<uint32_t> Frame(const std::string &name, const void *who, int item, const std::vector<ClxSprite> &sprites) const
	{
		const Surface out(pal);
		hd::ForgetView(out);
		{
			const hd::HeldItem held(who, item);
			for (size_t i = 0; i < sprites.size(); ++i)
				ClxDraw(out, { static_cast<int>(i) * 8, 3 }, sprites[i]);
		}
		hd::RequestDump(name);
		hd::Present(nullptr, pal);
		return DumpedFrame(dump, name);
	}
};

TEST(HdTest, HeldArtIsLetGoOfWhenTheHeroHoldsAnotherItem)
{
	const HeldPack pack("held_swap");
	ASSERT_TRUE(hd::Enabled());
	ASSERT_EQ(hd::Held().items, 2U);
	const hd::MemoryUse before = hd::Memory();
	const int hero = 0; // (the address names the hero)
	std::optional<OwnedClxSpriteList> stand = Owned(Sprite());
	std::optional<OwnedClxSpriteList> attack = Owned(Sprite());
	hd::RegisterSpriteList("PlrGFX\\Warrior\\WLS\\WLSWL.CL2", 0, *stand);
	hd::RegisterSpriteList("PlrGFX\\Warrior\\WLS\\WLSHT.CL2", 0, *attack);

	const std::vector<uint32_t> first = pack.Frame("c060", &hero, 60, { (*stand)[0], (*attack)[0] });
	ASSERT_FALSE(first.empty());
	EXPECT_TRUE(hd::Loaded(HeldPack::Held("c060", "wlswl")));
	EXPECT_TRUE(hd::Loaded(HeldPack::Held("c060", "wlsht")));
	EXPECT_EQ(hd::Memory().loads, before.loads + 2);
	EXPECT_EQ(hd::Held().frames, 2U);

	// The hero takes the other sword (same weapon class: its sprite files stay), as CalcPlrInv tells the layer, and stands about
	{
		const hd::HeldItem noted(&hero, 61);
	}
	EXPECT_EQ(hd::Held().frames, 0U) << "the frames let go of the last sword's art at once";
	const std::vector<uint32_t> other = pack.Frame("c061", &hero, 61, { (*stand)[0] });
	pack.EmptyFrames(20);
	EXPECT_FALSE(hd::Loaded(HeldPack::Held("c060", "wlswl")));
	EXPECT_FALSE(hd::Loaded(HeldPack::Held("c060", "wlsht"))) << "let go of, though the attack wasn't drawn again";
	EXPECT_TRUE(hd::Loaded(HeldPack::Held("c061", "wlswl")));
	EXPECT_FALSE(hd::Loaded(HeldPack::Held("c061", "wlsht"))) << "loaded only when drawn";
	EXPECT_TRUE(other != first);

	// The first sword again: loaded again, drawn exactly as before
	const std::vector<uint32_t> again = pack.Frame("c060-again", &hero, 60, { (*stand)[0], (*attack)[0] });
	EXPECT_TRUE(again == first);
	EXPECT_TRUE(hd::Loaded(HeldPack::Held("c060", "wlswl")));

	// A shield instead: the set's art, the sword's let go of
	const std::vector<uint32_t> shield = pack.Frame("shield", &hero, -1, { (*stand)[0], (*attack)[0] });
	pack.EmptyFrames(20);
	EXPECT_FALSE(hd::Loaded(HeldPack::Held("c060", "wlswl")));
	EXPECT_TRUE(hd::Loaded("sprite/plrgfx/warrior/wls/wlswl/0.0.png"));
	EXPECT_TRUE(shield != first);

	// Two heroes holding the same sword: one lets go, the other still draws it
	const int otherHero = 0;
	const OwnedClxSpriteList stand2 = Owned(Sprite());
	hd::RegisterSpriteList("PlrGFX\\Warrior\\WLS\\WLSWL.CL2", 0, stand2);
	pack.Frame("both", &hero, 61, { (*stand)[0] });
	pack.Frame("both2", &otherHero, 61, { stand2[0] });
	{
		const hd::HeldItem noted(&hero, 60);
	}
	pack.EmptyFrames(20);
	EXPECT_TRUE(hd::Loaded(HeldPack::Held("c061", "wlswl"))) << "the other hero still holds it";
	hd::LetGoOfHeldArt(&otherHero);
	pack.EmptyFrames(20);
	EXPECT_FALSE(hd::Loaded(HeldPack::Held("c061", "wlswl")));

	// The game frees the hero's sprite files (a level change, another weapon class): its held art goes with them
	pack.Frame("freed", &hero, 60, { (*stand)[0], (*attack)[0] });
	EXPECT_TRUE(hd::Loaded(HeldPack::Held("c060", "wlsht")));
	stand.reset();
	attack.reset();
	pack.EmptyFrames(20);
	EXPECT_FALSE(hd::Loaded(HeldPack::Held("c060", "wlswl")));
	EXPECT_FALSE(hd::Loaded(HeldPack::Held("c060", "wlsht")));
	EXPECT_EQ(hd::Held().frames, 0U);
}

TEST(HdTest, TileVariantFileNamesArtByPosition)
{
	EXPECT_EQ(hd::TileVariantFile("l1", 5, { 18, 23 }, 4, 0), "tile/l1/5@2-3.png");
	EXPECT_EQ(hd::TileVariantFile("l1", 5, { 18, 23 }, 0, 0), "") << "no variants asked for";
	EXPECT_EQ(hd::TileVariantFile("l1", 5, { -1, -1 }, 4, 0), "") << "no position";
	const std::string a = hd::TileVariantFile("l1", 5, { 18, 23 }, 0, 6);
	EXPECT_EQ(a, hd::TileVariantFile("l1", 5, { 18, 23 }, 0, 6)) << "the same position picks the same variant";
	std::array<bool, 6> seen {};
	for (int x = 16; x < 48; ++x) {
		for (int y = 16; y < 48; ++y) {
			const std::string f = hd::TileVariantFile("l1", 5, { x, y }, 0, 6);
			ASSERT_EQ(f.rfind("tile/l1/5@h", 0), 0U);
			seen[std::stoi(f.substr(11))] = true;
		}
	}
	for (int k = 0; k < 6; ++k)
		EXPECT_TRUE(seen[k]) << "variant " << k << " is used";
}

/** One pillar's HD image of one colour, ``w`` x ``h`` pixels. */
void WriteTileArt(const fs::path &pack, const std::string &file, SDL_Color c, int w, int h, std::ofstream &index)
{
	fs::create_directories((pack / file).parent_path());
	SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
	SDL_FillRect(s, nullptr, SDL_MapRGBA(s->format, c.r, c.g, c.b, 255));
	IMG_SavePNG(s, (pack / file).string().c_str());
	SDL_FreeSurface(s);
	index << "image " << file << " palettes/test.pal\n";
}

/** Draws pillar 7 of tileset "t" (one 64x32 row) at dungeon position ``tile`` in a pack with ``periodLine`` and returns the HD frame. */
SDL_Surface *DrawPillarAt(const std::string &name, const std::string &periodLine, Point tile)
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
		index << "scale 2\n" << periodLine;
		WriteTileArt(pack, "tile/t/7.png", FrontArt, 128, 64, index);
		WriteTileArt(pack, "tile/t/7@1-0.png", BackArt, 128, 64, index);
	}
	setenv("DEVILUTIONX_HD_PACK", pack.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP", dump.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP_EVERY", "1", 1);
	hd::Init();
	if (!hd::Enabled()) return nullptr;
	SDL_Surface *pal = SDL_CreateRGBSurfaceWithFormat(0, 64, 32, 8, SDL_PIXELFORMAT_INDEX8);
	SDL_SetPaletteColors(pal->format->palette, colors.data(), 0, 256);
	SDL_FillRect(pal, nullptr, 0);
	hd::SetTarget(pal);
	hd::SetTileset("t");
	{
		const Surface out(pal);
		const hd::PillarScope pillar(out, { 0, 31 }, 7, 1, tile);
		const uint8_t art = Own;
		uint8_t *dst = static_cast<uint8_t *>(pal->pixels) + 4 * pal->pitch + 4;
		*dst = Own;
		hd::Mark(dst, &art, 1);
	}
	hd::Present(nullptr, pal);
	SDL_Surface *frame = LoadFrame(dump);
	hd::SetTileset("");
	SDL_FreeSurface(pal);
	fs::remove_all(pack);
	return frame;
}

/**
 * Draws pillar 7 of tileset "t" lit at level 0, eight pixels of Own (an animated colour, drawn as OwnLit as Hell's rotated light table
 * may draw it) at (4..11, 4) and two of Other (drawn as itself) at (4..5, 8), with the cycled colours 1-31 when ``cycled``, and returns the HD frame.
 */
SDL_Surface *DrawCycledPillar(const std::string &name, const std::string &extra, bool cycled)
{
	const fs::path pack = fs::temp_directory_path() / ("devilutionx_hd_" + name);
	const fs::path dump = pack / "dump";
	fs::remove_all(pack);
	fs::create_directories(dump);
	const std::array<SDL_Color, 256> colors = ShadingPalette();
	{
		fs::create_directories(pack / "palettes");
		std::ofstream pal(pack / "palettes/test.pal", std::ios::binary);
		for (const SDL_Color &c : colors)
			pal << c.r << c.g << c.b;
		std::ofstream index(pack / "hd.txt");
		index << "scale 2\n" << extra;
		WriteTileArt(pack, "tile/t/7.png", FrontArt, 128, 64, index);
	}
	setenv("DEVILUTIONX_HD_PACK", pack.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP", dump.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP_EVERY", "1", 1);
	hd::Init();
	if (!hd::Enabled()) return nullptr;
	SDL_Surface *pal = SDL_CreateRGBSurfaceWithFormat(0, 64, 32, 8, SDL_PIXELFORMAT_INDEX8);
	SDL_SetPaletteColors(pal->format->palette, colors.data(), 0, 256);
	SDL_FillRect(pal, nullptr, 0);
	hd::SetTarget(pal);
	hd::SetTileset("t");
	hd::SetCycledColors(1, cycled ? 31 : 0);
	{
		const Surface out(pal);
		const hd::PillarScope pillar(out, { 0, 31 }, 7, 1);
		const auto draw = [&](int x, int y, uint8_t art, uint8_t written) {
			uint8_t *dst = static_cast<uint8_t *>(pal->pixels) + y * pal->pitch + x;
			*dst = written;
			hd::Mark(dst, &art, 1, 0);
		};
		for (int x = 4; x < 12; ++x)
			draw(x, 4, Own, OwnLit);
		draw(4, 8, Other, Other);
		draw(5, 8, Other, Other);
	}
	hd::Present(nullptr, pal);
	SDL_Surface *frame = LoadFrame(dump);
	hd::SetCycledColors(1, 0);
	hd::SetTileset("");
	SDL_FreeSurface(pal);
	fs::remove_all(pack);
	return frame;
}

TEST(HdTest, ColoursTheLevelAnimatesStayOutOfAPillarsLightLevel)
{
	// A pillar's grade is its light level's, over all the pillar pixels lit at that level: the blood walls of Hell are most of them, and drew
	// the rest of the pillar red and dark. Left out of the level's grade and graded on their own, every pixel is as "shading pixel" draws it
	SDL_Surface *pixel = DrawCycledPillar("cycled_pillar_pixel", "shading pixel\n", false);
	ASSERT_NE(pixel, nullptr);
	const SDL_Color own = At(pixel, 8, 8), other = At(pixel, 8, 16);
	ExpectColor(other, FrontArt.r, FrontArt.g, FrontArt.b, "shading pixel: Other as it is");
	SDL_FreeSurface(pixel);

	SDL_Surface *before = DrawCycledPillar("cycled_pillar_none", "", false);
	ASSERT_NE(before, nullptr);
	const SDL_Color skewed = At(before, 8, 16);
	EXPECT_FALSE(skewed.r == FrontArt.r && skewed.g == FrontArt.g && skewed.b == FrontArt.b) << "without the cycled colours Other is pulled off";
	SDL_FreeSurface(before);

	SDL_Surface *frame = DrawCycledPillar("cycled_pillar", "", true);
	ASSERT_NE(frame, nullptr);
	ExpectColor(At(frame, 8, 16), other.r, other.g, other.b, "Other: as shading pixel draws it");
	ExpectColor(At(frame, 8, 8), own.r, own.g, own.b, "Own: as shading pixel draws it");
	ExpectColor(At(frame, 22, 9), own.r, own.g, own.b, "Own, the last pixel: as shading pixel draws it");
	SDL_FreeSurface(frame);
}

// See-through draws (hd.hpp, "See-through"): paletteTransparencyLookup gives the pair the index Blend, a colour of neither
constexpr uint8_t Blend = 99;
constexpr SDL_Color BlendColor = { 150, 75, 75, 255 };

/** A CLX list of one 4x4 sprite, every pixel ``index``. */
std::vector<uint8_t> SpriteOf(uint8_t index)
{
	std::vector<uint8_t> data = { 1, 0, 0, 0, 12, 0, 0, 0, 38, 0, 0, 0, 6, 0, 4, 0, 4, 0 };
	for (int row = 0; row < 4; ++row)
		data.insert(data.end(), { 0xFC, index, index, index, index });
	return data;
}

/**
 * Draws a sprite of Other with the art BackArt at lo pixels (4..7, 1..4), and over it, blended, a sprite of Own with the art FrontArt: through
 * ``light`` (one of the game's light tables: ClxDrawBlendedTRN), or plainly (ClxDrawBlended). paletteTransparencyLookup gives Blend for the
 * pair. Returns the HD frame (scale 2); the PalSurface comes back in ``palOut``.
 */
SDL_Surface *DrawBlendedSprites(const std::string &name, const uint8_t *light, int width, SDL_Surface **palOut)
{
	const fs::path pack = fs::temp_directory_path() / ("devilutionx_hd_" + name);
	const fs::path dump = pack / "dump";
	fs::remove_all(pack);
	fs::create_directories(dump);
	std::array<SDL_Color, 256> colors = ShadingPalette();
	colors[Blend] = BlendColor;
	{
		fs::create_directories(pack / "palettes");
		std::ofstream pal(pack / "palettes/test.pal", std::ios::binary);
		for (const SDL_Color &c : colors)
			pal << c.r << c.g << c.b;
		std::ofstream index(pack / "hd.txt");
		index << "scale 2\n";
		WriteArt(pack, "sprite/" + name + "/back/0.0.png", BackArt, index);
		WriteArt(pack, "sprite/" + name + "/front/0.0.png", FrontArt, index);
	}
	setenv("DEVILUTIONX_HD_PACK", pack.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP", dump.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP_EVERY", "1", 1);
	hd::Init();
	EXPECT_TRUE(hd::Enabled());
	hd::SetLightTables(light, light != nullptr ? 256 : 0);
	const uint8_t front = light != nullptr ? light[Own] : Own;
	const uint8_t saved[2] = { paletteTransparencyLookup[Other][front], paletteTransparencyLookup[front][Other] };
	paletteTransparencyLookup[Other][front] = paletteTransparencyLookup[front][Other] = Blend;

	// Not the size of any other test's surface, so that the provenance plane starts clean
	SDL_Surface *pal = SDL_CreateRGBSurfaceWithFormat(0, width, 9, 8, SDL_PIXELFORMAT_INDEX8);
	SDL_SetPaletteColors(pal->format->palette, colors.data(), 0, 256);
	SDL_FillRect(pal, nullptr, BackgroundIndex);
	hd::SetTarget(pal);
	const Surface out(pal);
	static std::vector<uint8_t> back, over;
	back = SpriteOf(Other);
	over = SpriteOf(Own);
	hd::RegisterSpriteList(name + "\\Back.CL2", 0, ClxSpriteList { back.data() });
	hd::RegisterSpriteList(name + "\\Front.CL2", 0, ClxSpriteList { over.data() });
	ClxDraw(out, { 4, 4 }, ClxSpriteList { back.data() }[0]);
	if (light != nullptr)
		ClxDrawBlendedTRN(out, { 4, 4 }, ClxSpriteList { over.data() }[0], light);
	else
		ClxDrawBlended(out, { 4, 4 }, ClxSpriteList { over.data() }[0]);
	hd::Present(nullptr, pal);
	SDL_Surface *frame = LoadFrame(dump);
	paletteTransparencyLookup[Other][front] = saved[0];
	paletteTransparencyLookup[front][Other] = saved[1];
	hd::SetLightTables(nullptr, 0);
	fs::remove_all(pack);
	*palOut = pal;
	return frame;
}

TEST(HdTest, ABlendedSpriteIsDrawnHalfOverWhatIsBehindItInTrueColour)
{
	// The game writes Blend, the palette colour nearest the mean of the two. The layer used to grade the front art by the pair (Own, Blend):
	// times 0.75, 1.5, 1.5 per channel, so art of colours of its own came out tinted (yellow and purple specks on see-through walls), and drawn
	// whole, the sprite behind gone. Now: the front art as the same draw opaque grades it, half over the art behind
	SDL_Surface *pal = nullptr;
	SDL_Surface *frame = DrawBlendedSprites("blendplain", nullptr, 30, &pal);
	ASSERT_NE(frame, nullptr);
	EXPECT_EQ(static_cast<uint8_t *>(pal->pixels)[2 * pal->pitch + 5], Blend) << "the game's own picture is as it was";
	const SDL_Color half = { static_cast<uint8_t>((FrontArt.r + BackArt.r) / 2), static_cast<uint8_t>((FrontArt.g + BackArt.g) / 2), static_cast<uint8_t>((FrontArt.b + BackArt.b) / 2), 255 };
	ExpectColor(At(frame, 8, 2), half.r, half.g, half.b, "half the front art over the back art");
	ExpectColor(At(frame, 15, 9), half.r, half.g, half.b, "half the front art over the back art, the last pixel");
	ExpectColor(At(frame, 7, 2), Background.r, Background.g, Background.b, "beside the sprites");
	SDL_FreeSurface(frame);
	SDL_FreeSurface(pal);

	// Through a light table that draws Own at half (OwnLit): the front art is graded by the lit index, as the opaque draw would be
	std::array<uint8_t, 256> light = Identity();
	light[Own] = OwnLit;
	frame = DrawBlendedSprites("blendlit", light.data(), 34, &pal);
	ASSERT_NE(frame, nullptr);
	EXPECT_EQ(static_cast<uint8_t *>(pal->pixels)[2 * pal->pitch + 5], Blend) << "the game's own picture is as it was";
	ExpectColor(At(frame, 8, 2), (FrontArt.r / 2 + BackArt.r) / 2, (FrontArt.g / 2 + BackArt.g) / 2, (FrontArt.b / 2 + BackArt.b) / 2,
	    "the front art at half light, half over the back art");
	SDL_FreeSurface(frame);
	SDL_FreeSurface(pal);
}

/**
 * Draws pillar 7 of tileset "t" lit at level 0 (art FrontArt) as dun_render draws a see-through wall when ``blended``: eight pixels of Own
 * drawn opaque at (4..11, 4), and eight blended at (4..11, 8) over the background, written as Blend. Without ``blended`` the second row is
 * marked as an opaque draw writing Blend (the layer's view of it before see-through draws were told apart). Returns the HD frame.
 */
SDL_Surface *DrawSeeThroughPillar(const std::string &name, bool blended, int height)
{
	const fs::path pack = fs::temp_directory_path() / ("devilutionx_hd_" + name);
	const fs::path dump = pack / "dump";
	fs::remove_all(pack);
	fs::create_directories(dump);
	std::array<SDL_Color, 256> colors = Palette();
	colors[Blend] = BlendColor;
	{
		fs::create_directories(pack / "palettes");
		std::ofstream pal(pack / "palettes/test.pal", std::ios::binary);
		for (const SDL_Color &c : colors)
			pal << c.r << c.g << c.b;
		std::ofstream index(pack / "hd.txt");
		index << "scale 2\n";
		WriteTileArt(pack, "tile/t/7.png", FrontArt, 128, 64, index);
	}
	setenv("DEVILUTIONX_HD_PACK", pack.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP", dump.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP_EVERY", "1", 1);
	hd::Init();
	if (!hd::Enabled()) return nullptr;
	SDL_Surface *pal = SDL_CreateRGBSurfaceWithFormat(0, 64, height, 8, SDL_PIXELFORMAT_INDEX8);
	SDL_SetPaletteColors(pal->format->palette, colors.data(), 0, 256);
	SDL_FillRect(pal, nullptr, BackgroundIndex);
	hd::SetTarget(pal);
	hd::SetTileset("t");
	{
		const Surface out(pal);
		const hd::PillarScope pillar(out, { 0, 31 }, 7, 1);
		uint8_t art[8];
		std::fill_n(art, 8, Own);
		uint8_t *opaque = static_cast<uint8_t *>(pal->pixels) + 4 * pal->pitch + 4;
		std::fill_n(opaque, 8, Own);
		hd::Mark(opaque, art, 8, 0);
		uint8_t *seeThrough = static_cast<uint8_t *>(pal->pixels) + 8 * pal->pitch + 4;
		if (blended) hd::BeforeBlend(seeThrough, 8);
		std::fill_n(seeThrough, 8, Blend);
		hd::Mark(seeThrough, art, 8, 0);
		if (blended) hd::MarkBlend(seeThrough, art, 8); // lit at level 0: the art's own index
	}
	hd::Present(nullptr, pal);
	SDL_Surface *frame = LoadFrame(dump);
	hd::SetTileset("");
	SDL_FreeSurface(pal);
	fs::remove_all(pack);
	return frame;
}

TEST(HdTest, ASeeThroughWallShowsWhatIsBehindItAndLeavesItsLightLevelAlone)
{
	// The pairs (Own, Blend) of the see-through pixels went into the light level's grade and pulled the opaque part of the wall off too
	SDL_Surface *before = DrawSeeThroughPillar("seethrough_none", false, 30);
	ASSERT_NE(before, nullptr);
	const SDL_Color skewed = At(before, 8, 8);
	EXPECT_FALSE(skewed.r == FrontArt.r && skewed.g == FrontArt.g && skewed.b == FrontArt.b) << "marked as opaque, the blended row pulls the rest off";
	SDL_FreeSurface(before);

	SDL_Surface *frame = DrawSeeThroughPillar("seethrough", true, 29);
	ASSERT_NE(frame, nullptr);
	ExpectColor(At(frame, 8, 8), FrontArt.r, FrontArt.g, FrontArt.b, "the opaque part: lit by nothing, as it is");
	ExpectColor(At(frame, 8, 16), (FrontArt.r + Background.r) / 2, (FrontArt.g + Background.g) / 2, (FrontArt.b + Background.b) / 2,
	    "the see-through part: half over what was behind it (no art there: its colour)");
	ExpectColor(At(frame, 23, 17), (FrontArt.r + Background.r) / 2, (FrontArt.g + Background.g) / 2, (FrontArt.b + Background.b) / 2,
	    "the see-through part, the last pixel");
	SDL_FreeSurface(frame);
}

TEST(HdTest, APillarDrawsItsArtForThePositionWhenThePackHasSome)
{
	SDL_Surface *variant = DrawPillarAt("tile_variant", "tileperiod t 2\n", { 17, 20 }); // 17 % 2 = 1, 20 % 2 = 0: 7@1-0
	ASSERT_NE(variant, nullptr);
	ExpectColor(At(variant, 8, 8), BackArt.r, BackArt.g, BackArt.b, "the art for x % 2 = 1, y % 2 = 0");
	SDL_FreeSurface(variant);

	SDL_Surface *fallback = DrawPillarAt("tile_fallback", "tileperiod t 2\n", { 16, 20 }); // 7@0-0 is not in the pack
	ASSERT_NE(fallback, nullptr);
	ExpectColor(At(fallback, 8, 8), FrontArt.r, FrontArt.g, FrontArt.b, "no art for this position: the pillar's own");
	SDL_FreeSurface(fallback);

	SDL_Surface *off = DrawPillarAt("tile_off", "", { 17, 20 }); // the pack does not ask for art by position
	ASSERT_NE(off, nullptr);
	ExpectColor(At(off, 8, 8), FrontArt.r, FrontArt.g, FrontArt.b, "without tileperiod the variant files are not looked at");
	SDL_FreeSurface(off);
}

} // namespace
} // namespace devilution
