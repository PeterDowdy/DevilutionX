#include "engine/render/hd.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
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

} // namespace
} // namespace devilution
