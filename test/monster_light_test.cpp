#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "engine/render/hd.hpp"
#include "lighting.h"
#include "monster.h"
#include "tables/monstdat.h"

namespace devilution {
namespace {

namespace fs = std::filesystem;

#ifdef _WIN32
int setenv(const char *name, const char *value, int /*overwrite*/)
{
	return _putenv_s(name, value);
}
#endif

constexpr int Radius = 6;

/** "zombie\\zombie" -> "monsters/zombie/zombie": a type's sprite path as the pack's "light" lines name it. */
std::string LightKey(_monster_id type)
{
	std::string who = "monsters/";
	for (const char *c = MonstersData[type].spritePath(); *c != '\0'; ++c)
		who += *c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));
	return who;
}

/** A level with two monster types, and an HD pack that gives the first of them a light ("light" line of hd.txt), and ``extra`` lines. */
void LitLevel(const std::string &extra = "")
{
	LoadMonsterData();
	InitLevelMonsters();
	LevelMonsterTypes[0].type = MT_NZOMBIE;
	LevelMonsterTypes[1].type = MT_RFALLSP;
	LevelMonsterTypeCount = 2;
	InitLighting();

	const std::string who = LightKey(MT_NZOMBIE);
	const fs::path pack = fs::temp_directory_path() / "devilutionx_hd_monster_light";
	fs::remove_all(pack);
	fs::create_directories(pack);
	{
		std::ofstream index(pack / "hd.txt");
		index << "scale 2\nlight " << who << " " << Radius << "\n"
		      << extra;
	}
	setenv("DEVILUTIONX_HD_PACK", pack.string().c_str(), 1);
	hd::Init();
	ASSERT_EQ(hd::MonsterLight(MonstersData[MT_NZOMBIE].spritePath(), {}), Radius);
	ASSERT_EQ(hd::MonsterLight(MonstersData[MT_RFALLSP].spritePath(), {}), 0);
}

Monster &Place(uint8_t typeIndex, Point position)
{
	Monster &monster = Monsters[ActiveMonsters[ActiveMonsterCount++]];
	monster.levelType = typeIndex;
	monster.uniqueType = UniqueMonsterType::None;
	monster.flags = 0;
	monster.position.tile = position;
	monster.lightId = NO_LIGHT;
	return monster;
}

bool Lit(const Monster &monster)
{
	if (monster.lightId == NO_LIGHT)
		return false;
	const auto end = ActiveLights.begin() + ActiveLightCount;
	return std::find(ActiveLights.begin(), end, monster.lightId) != end && !Lights[monster.lightId].isInvalid;
}

int ValidLights()
{
	int valid = 0;
	for (int i = 0; i < ActiveLightCount; i++)
		valid += Lights[ActiveLights[i]].isInvalid ? 0 : 1;
	return valid;
}

TEST(MonsterLight, AMonsterOfALitTypeIsGivenALightAndNeverLightZero)
{
	LitLevel();
	Monster &zombie = Place(0, { 20, 20 });
	Monster &other = Place(0, { 24, 20 });
	Monster &fallen = Place(1, { 28, 20 });
	fallen.lightId = 3; // what a save made with another pack might name
	GiveTypeLights({});

	EXPECT_TRUE(Lit(zombie));
	EXPECT_TRUE(Lit(other));
	EXPECT_NE(zombie.lightId, 0) << "saves read a monster's light 0 as none";
	EXPECT_NE(other.lightId, 0);
	EXPECT_NE(zombie.lightId, other.lightId);
	EXPECT_EQ(Lights[zombie.lightId].radius, Radius);
	EXPECT_EQ(Lights[zombie.lightId].position.tile, Point(20, 20));
	EXPECT_EQ(fallen.lightId, NO_LIGHT) << "a type the pack doesn't light keeps none";
	EXPECT_EQ(ValidLights(), 2);
}

TEST(MonsterLight, TypeLightsLeaveRoomInThePool)
{
	LitLevel();
	std::vector<Monster *> zombies;
	for (int i = 0; i < MAXLIGHTS + 4; i++)
		zombies.push_back(&Place(0, { 20 + i, 20 }));
	GiveTypeLights({});

	std::set<int> lights;
	for (const Monster *zombie : zombies) {
		if (zombie->lightId == NO_LIGHT)
			continue;
		EXPECT_TRUE(Lit(*zombie));
		EXPECT_TRUE(lights.insert(zombie->lightId).second) << "two monsters with light " << static_cast<int>(zombie->lightId);
	}
	EXPECT_GT(lights.size(), 8U);
	EXPECT_EQ(ActiveLightCount, MAXLIGHTS - TypeLightsKeptFree);
	EXPECT_EQ(zombies.back()->lightId, NO_LIGHT) << "the pool is shared with the players' and the spells' lights";
	EXPECT_NE(AddLight({ 30, 30 }, 10), NO_LIGHT) << "a player's light still has a place";
}

TEST(MonsterLight, ALevelLoadedAgainGivesItsTypeLightsToTheMonstersOfTheSave)
{
	LitLevel();
	// The level as it is made again on the way in: every monster of the lit type has a light
	for (int i = 0; i < 3; i++)
		Place(0, { 20 + 4 * i, 20 });
	Place(1, { 40, 20 });
	GiveTypeLights({});
	const std::vector<int> lights = TakeTypeLights();
	ASSERT_EQ(lights.size(), 3U);
	for (size_t i = 0; i < ActiveMonsterCount; i++)
		EXPECT_EQ(Monsters[ActiveMonsters[i]].lightId, NO_LIGHT);

	// The save's monsters take their place: one zombie has died, one has moved and names a light of the last visit,
	// one was made Berserk (its light is UpdateMonsterLights's to give), and the other type names a light too
	InitLevelMonsters();
	LevelMonsterTypeCount = 2;
	Monster &moved = Place(0, { 50, 50 });
	moved.lightId = 17;
	Monster &berserk = Place(0, { 60, 60 });
	berserk.flags = MFLAG_BERSERK;
	berserk.lightId = 5;
	Monster &fallen = Place(1, { 40, 20 });
	fallen.lightId = lights[0];
	GiveTypeLights(lights);

	EXPECT_TRUE(Lit(moved));
	EXPECT_EQ(moved.lightId, lights[0]);
	EXPECT_EQ(Lights[moved.lightId].position.tile, Point(50, 50));
	EXPECT_EQ(Lights[moved.lightId].radius, Radius);
	EXPECT_EQ(berserk.lightId, 5) << "left for UpdateMonsterLights";
	EXPECT_EQ(fallen.lightId, NO_LIGHT) << "a light of the last visit is not this monster's";
	EXPECT_TRUE(Lights[lights[1]].isInvalid) << "the light of the zombie that died goes out";
	EXPECT_TRUE(Lights[lights[2]].isInvalid);
	EXPECT_EQ(ValidLights(), 1);
}

const UniqueMonsterData &UniqueNamed(const char *name)
{
	const auto found = std::find_if(UniqueMonstersData.begin(), UniqueMonstersData.end(), [&](const UniqueMonsterData &u) { return u.mName == name; });
	EXPECT_NE(found, UniqueMonstersData.end()) << name;
	return *found;
}

TEST(MonsterLight, AUniquesOwnLightIsHisAloneAndNeverLightsHisType)
{
	// Blightfire is a Fire Clan archer (goatbow\goatb through goatbow\red) with a TRN of his own: "@red+blf", as his art is keyed
	LoadMonsterData(); // (for the keys; LitLevel loads it again)
	const std::string archer = LightKey(MT_RGOATBW);
	LitLevel("light " + archer + "@red+blf 9\n");
	const UniqueMonsterData &blightfire = UniqueNamed("Blightfire");
	ASSERT_EQ(blightfire.mtype, MT_RGOATBW);
	const MonsterData &fireClan = MonstersData[MT_RGOATBW];

	EXPECT_EQ(hd::UniqueMonsterLight(fireClan.spritePath(), fireClan.trnFile, blightfire.mTrnName, blightfire.mName), 9);
	EXPECT_EQ(UniqueMonsterLightRadius(fireClan, blightfire), 9) << "the unique's own light wins";
	EXPECT_EQ(hd::MonsterLight(fireClan.spritePath(), fireClan.trnFile), 0) << "a unique's light is not his type's";
	EXPECT_EQ(hd::MonsterLight(fireClan.spritePath(), {}), 0) << "nor his body's";
	const MonsterData &goat = MonstersData[MT_NGOATBW]; // the same body, another clan
	EXPECT_EQ(hd::UniqueMonsterLight(goat.spritePath(), goat.trnFile, blightfire.mTrnName, blightfire.mName), 0);

	// An ordinary Fire Clan archer beside him has no light; a zombie (lit type) still has its own
	LevelMonsterTypes[1].type = MT_RGOATBW;
	Monster &archerMonster = Place(1, { 20, 20 });
	Monster &zombie = Place(0, { 24, 20 });
	GiveTypeLights({});
	EXPECT_EQ(archerMonster.lightId, NO_LIGHT);
	EXPECT_TRUE(Lit(zombie));
	EXPECT_EQ(Lights[zombie.lightId].radius, Radius);
}

TEST(MonsterLight, AUniqueTakesHisTypesLightOnlyWhenItIsBrighterThanAUniquesOwn)
{
	// No line of his own: a unique of the lit zombie type (Soulpus) takes the type's light like his art does, one of an unlit type keeps
	// the game's; a dim type light doesn't dim a unique
	LoadMonsterData();
	LitLevel("light " + LightKey(MT_RGOATBW) + "@red 2\n");
	EXPECT_EQ(UniqueMonsterLightRadius(MonstersData[MT_NZOMBIE], UniqueNamed("Soulpus")), Radius);
	EXPECT_EQ(UniqueMonsterLightRadius(MonstersData[MT_RGOATBW], UniqueNamed("Blightfire")), UniqueLightRadius);
	EXPECT_EQ(UniqueMonsterLightRadius(MonstersData[MT_BLINK], UniqueNamed("Moonbender")), UniqueLightRadius);
	EXPECT_EQ(hd::MonsterLight(MonstersData[MT_RGOATBW].spritePath(), MonstersData[MT_RGOATBW].trnFile), 2) << "the type's own";
}

TEST(MonsterLight, AUniquesSlugLightWinsOverTheLightOfHisSharedColours)
{
	// Moonbender and Wrathraven are both MT_BLINK through the shared "general" TRN: "+general" lights both, "+general~moonbender" him alone
	LoadMonsterData();
	const MonsterData &blink = MonstersData[MT_BLINK];
	ASSERT_TRUE(blink.trnFile.empty()) << "a type without a TRN of its own: the key starts \"@+\"";
	const std::string swap = LightKey(MT_BLINK) + "@+general";
	LitLevel("light " + swap + " 5\nlight " + swap + "~moonbender 7\n");
	EXPECT_EQ(UniqueMonsterLightRadius(blink, UniqueNamed("Moonbender")), 7);
	EXPECT_EQ(UniqueMonsterLightRadius(blink, UniqueNamed("Wrathraven")), 5);
	EXPECT_EQ(hd::MonsterLight(blink.spritePath(), blink.trnFile), 0) << "neither lights the type";
	EXPECT_EQ(UniqueMonsterLightRadius(MonstersData[MT_NZOMBIE], UniqueNamed("Soulpus")), Radius) << "another type's general unique";
}

} // namespace
} // namespace devilution
