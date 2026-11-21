/**
 * Tests for the 8-player build: per-player tables, town portal spots, player colours and the hit mask.
 */
#include <set>

#include <gtest/gtest.h>

#include "engine/lighting_defs.hpp"
#include "engine/palette.h"
#include "engine/player_colors.hpp"
#include "monster.h"
#include "multi.h"
#include "player.h"
#include "portal.h"

using namespace devilution;

namespace {

TEST(Players8, TablesHoldEveryPlayer)
{
	EXPECT_EQ(MAX_PLRS, 8);
	EXPECT_EQ(MAXPORTAL, MAX_PLRS);
	EXPECT_EQ(MAXVISION, MAX_PLRS);
	EXPECT_GE(NumPlayerColors, static_cast<size_t>(MAX_PLRS));
}

TEST(Players8, TownPortalSpots)
{
	// Players 1-4 keep the original spots.
	EXPECT_EQ(GetPortalTownPosition(0), Point(57, 40));
	EXPECT_EQ(GetPortalTownPosition(1), Point(59, 40));
	EXPECT_EQ(GetPortalTownPosition(2), Point(61, 40));
	EXPECT_EQ(GetPortalTownPosition(3), Point(63, 40));
	// Players 5-8 on a second row; no two portals, nor a portal and an arrival square (portal + 1,1), share a square.
	std::set<std::pair<int, int>> used;
	for (size_t id = 0; id < MAX_PLRS; id++) {
		const Point portal = GetPortalTownPosition(id);
		const Point arrival = portal + Displacement { 1, 1 };
		EXPECT_TRUE(used.emplace(portal.x, portal.y).second) << "player " << id;
		EXPECT_TRUE(used.emplace(arrival.x, arrival.y).second) << "player " << id;
	}
	EXPECT_EQ(GetPortalTownPosition(4), Point(57, 43));
	EXPECT_EQ(GetPortalTownPosition(7), Point(63, 43));
}

TEST(Players8, ColoursFirstFourAsOriginal)
{
	EXPECT_EQ(PlayerMapColors[0], PAL8_ORANGE + 1);
	EXPECT_EQ(PlayerMapColors[1], PAL8_YELLOW + 1);
	EXPECT_EQ(PlayerMapColors[2], PAL8_RED + 1);
	EXPECT_EQ(PlayerMapColors[3], PAL8_BLUE + 1);
	EXPECT_EQ(PlayerOutlineColors[0], PAL16_ORANGE + 7);
	EXPECT_EQ(PlayerOutlineColors[1], PAL16_YELLOW + 7);
	EXPECT_EQ(PlayerOutlineColors[2], PAL16_RED + 7);
	EXPECT_EQ(PlayerOutlineColors[3], PAL16_BLUE + 7);
}

TEST(Players8, ColoursDistinctAndFixed)
{
	const std::set<uint8_t> map(PlayerMapColors.begin(), PlayerMapColors.end());
	const std::set<uint8_t> outline(PlayerOutlineColors.begin(), PlayerOutlineColors.end());
	EXPECT_EQ(map.size(), NumPlayerColors);
	EXPECT_EQ(outline.size(), NumPlayerColors);
	// Indices below 128 change with the level palette.
	for (size_t id = 0; id < NumPlayerColors; id++) {
		EXPECT_GE(PlayerMapColors[id], 128) << "player " << id;
		EXPECT_GE(PlayerOutlineColors[id], 128) << "player " << id;
	}
}

TEST(Players8, ExperienceTags)
{
	for (size_t id = 0; id < 4; id++) {
		EXPECT_EQ(PlayerTagFrame(id), id + 1);
		EXPECT_EQ(PlayerTagTranslation(id), nullptr);
	}
	std::set<uint8_t> lightestShades;
	for (size_t id = 4; id < MAX_PLRS; id++) {
		EXPECT_EQ(PlayerTagFrame(id), 1U);
		const uint8_t *trn = PlayerTagTranslation(id);
		ASSERT_NE(trn, nullptr);
		for (unsigned i = 0; i < 256; i++) {
			if (i >= PAL8_ORANGE + 1 && i <= PAL8_ORANGE + 3)
				EXPECT_NE(trn[i], i) << "player " << id << " index " << i;
			else
				EXPECT_EQ(trn[i], i) << "player " << id << " index " << i;
		}
		lightestShades.insert(trn[PAL8_ORANGE + 1]);
	}
	EXPECT_EQ(lightestShades.size(), 4U);
}

TEST(Players8, HitMaskHoldsPlayerEight)
{
	Players.resize(MAX_PLRS);
	Monster &monster = Monsters[0];
	monster.whoHit = 0;
	monster.tag(Players[7]);
	monster.tag(Players[0]);
	EXPECT_EQ(monster.whoHit, 0x81);
	unsigned tagged = 0;
	for (size_t i = 0; i < Players.size(); i++) {
		if (((1U << i) & monster.whoHit) != 0)
			tagged++;
	}
	EXPECT_EQ(tagged, 2U);
	Players.clear();
}

} // namespace
