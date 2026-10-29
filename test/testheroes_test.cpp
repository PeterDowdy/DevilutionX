/**
 * Writes demo heroes for the HD remaster: one per class, maximum level and stats, every spell at level 15 and a kit that shows every
 * player animation (unarmed, shield, sword, sword + shield, mace, mace + shield, axe, bow, staff; light, medium and heavy armour: medium and
 * heavy are worn or carried, light is no chest armour).
 *
 * Opt-in: skipped unless TEST_HEROES_OUT (save directory to write) and TEST_HEROES_DATA (folder of diabdat.mpq) are set.
 *   TEST_HEROES_OUT=DIR TEST_HEROES_DATA=DIR TEST_HEROES_ASSETS=BUILD/assets/ ./testheroes_test
 * Saves are single_<N>.sv with N = TEST_HEROES_FIRST (default 1) and up.
 * TEST_HEROES_HELLFIRE=1 (needs TEST_HEROES_ASSETS: the hf mod tables are read from BUILD/mods/hf/): Hellfire heroes, the same three plus a monk, as single_<N>.hsv
 * (the extension the game reads with the hf mod active).
 *
 * TestHeroes.ConvertToHellfire: TEST_HEROES_CONVERT_FROM=SAVEDIR (with TEST_HEROES_OUT/DATA/ASSETS) reads single_<N>.sv there
 * (N = TEST_HEROES_CONVERT, default 0) as Diablo and writes the same hero as a Hellfire single_<N>.hsv into TEST_HEROES_OUT. Hero only:
 * Hellfire numbers the level files differently, so the Diablo game in progress is not carried over (the hero starts a new game).
 */
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "cursor.h"
#include "engine/assets.hpp"
#include "init.hpp"
#include "inv.h"
#include "items.h"
#include "loadsave.h"
#include "menu.h"
#include "pfile.h"
#include "player.h"
#include "spells.h"
#include "tables/playerdat.hpp"
#include "tables/spelldat.h"
#include "utils/paths.h"
#include "utils/str_cat.hpp"
#include "utils/utf8.hpp"

namespace devilution {
namespace {

_item_indexes ItemByName(std::string_view name)
{
	for (size_t i = 0; i < AllItemsList.size(); i++) {
		if (AllItemsList[i].iName == name && AllItemsList[i].dropRate != 0)
			return static_cast<_item_indexes>(i);
	}
	return IDI_NONE;
}

void Give(Player &player, std::string_view name)
{
	const _item_indexes idx = ItemByName(name);
	ASSERT_NE(idx, IDI_NONE) << name;
	Item item;
	InitializeItem(item, idx);
	GenerateNewSeed(item);
	item.updateRequiredStatsCacheForPlayer(player);
	const bool worn = AutoEquip(player, item);
	EXPECT_TRUE(worn || AutoPlaceItemInBelt(player, item, true) || AutoPlaceItemInInventory(player, item)) << name;
	if (std::getenv("TEST_HEROES_VERBOSE") != nullptr)
		std::cout << "  " << name << (worn ? " worn" : " packed") << " (" << item._iName << ", str " << static_cast<int>(item._iMinStr) << ")\n";
}

void MakeHero(uint32_t saveNumber, HeroClass heroClass, const char *name, std::string_view armour)
{
	_uiheroinfo info {};
	info.saveNumber = saveNumber;
	info.heroclass = heroClass;
	CopyUtf8(info.name, name, sizeof(info.name));
	ASSERT_TRUE(pfile_ui_save_create(&info));
	gSaveNumber = saveNumber;
	Player &player = *MyPlayer;

	// Level 50: the hit points and mana a level-up adds (NextPlrLevel without its sound and panel side effects)
	const int levels = player.getMaxCharacterLevel() - player.getCharacterLevel();
	player.setCharacterLevel(player.getMaxCharacterLevel() - 1);
	player._pExperience = player.getNextExperienceThreshold();
	player.setCharacterLevel(player.getMaxCharacterLevel());
	const int life = player.getClassAttributes().lvlLife * levels;
	const int mana = player.getClassAttributes().lvlMana * levels;
	player._pMaxHPBase += life;
	player._pHPBase = player._pMaxHPBase;
	player._pMaxManaBase += mana;
	player._pManaBase = player._pMaxManaBase;

	// Every base attribute at the class maximum, nothing left to spend (MyPlayer unset: the Modify functions would send a net command)
	MyPlayer = nullptr;
	ModifyPlrStr(player, player.GetMaximumAttributeValue(CharacterAttribute::Strength) - player._pBaseStr);
	ModifyPlrMag(player, player.GetMaximumAttributeValue(CharacterAttribute::Magic) - player._pBaseMag);
	ModifyPlrDex(player, player.GetMaximumAttributeValue(CharacterAttribute::Dexterity) - player._pBaseDex);
	ModifyPlrVit(player, player.GetMaximumAttributeValue(CharacterAttribute::Vitality) - player._pBaseVit);
	player._pStatPts = 0;
	MyPlayer = &player;

	// Every book spell at level 15 (Firebolt -> fire cast, Lightning -> lightning cast, the rest -> the plain cast)
	for (int8_t s = static_cast<int8_t>(SpellID::Firebolt); s <= static_cast<int8_t>(SpellID::LAST); s++) {
		const auto spell = static_cast<SpellID>(s);
		if (!IsValidSpell(spell) || GetSpellBookLevel(spell) <= 0)
			continue;
		player._pMemSpells |= GetSpellBitmask(spell);
		player._pSplLvl[s] = 15;
	}
	player._pRSpell = SpellID::Firebolt;
	player._pRSplType = SpellType::Spell;

	// The kit: start clean, wear the chosen armour, sword + buckler in hand, the rest in the pack (low requirements, every class can use it)
	for (auto &item : player.InvBody)
		item.clear();
	for (auto &item : player.InvList)
		item.clear();
	for (auto &item : player.SpdList)
		item.clear();
	memset(&player.InvGrid, 0, sizeof(player.InvGrid));
	player._pNumInv = 0;
	player._pGold = 0;
	CalcPlrInv(player, false);

	InitCursor();
	Give(player, armour);
	Give(player, "Short Sword");
	Give(player, "Buckler");
	for (const char *other : { "Ring Mail", "Breast Plate" }) { // light armour = no chest armour (take it off), the pack is too small for a third
		if (armour != other)
			Give(player, other);
	}
	for (const char *item : { "Mace", "Small Axe", "Short Bow", "Short Staff" })
		Give(player, item);
	for (int i = 0; i < 8; i++) {
		Item potion;
		InitializeItem(potion, IDI_FULLHEAL);
		GenerateNewSeed(potion);
		AutoPlaceItemInBelt(player, potion, true);
	}
	FreeCursor();

	CalcPlrInv(player, false);
	player._pHitPoints = player._pMaxHP;
	player._pMana = player._pMaxMana;
	pfile_write_hero(/*writeGameData=*/false);
	std::cout << name << ": level " << static_cast<int>(player.getCharacterLevel()) << ", str/mag/dex/vit " << player._pBaseStr << '/'
	          << player._pBaseMag << '/' << player._pBaseDex << '/' << player._pBaseVit << ", life " << (player._pMaxHP >> 6) << ", mana "
	          << (player._pMaxMana >> 6) << ", items " << player._pNumInv << " in the pack -> single_" << saveNumber << ".sv\n";
}

void LoadTables()
{
	LoadSpellData();
	LoadPlayerDataFiles();
	LoadMonsterData();
	LoadItemData();
}

// What the hf mod does in the game: the Hellfire archives, its table overrides (txtdata), Hellfire on
void EnableHellfire(const char *assets)
{
	ASSERT_NE(assets, nullptr) << "Hellfire needs TEST_HEROES_ASSETS";
	LoadHellfireArchives();
	OverridePaths.emplace_back(StrCat(assets, "../mods/hf/")); // the build keeps the hf mod beside assets/
	gbIsHellfire = true;
	gbIsHellfireSaveGame = true;
	LoadTables();
}

// The game's archives and single-player state with save files in `pref`, Diablo tables loaded
void SetUpGame(const char *data, const char *assets, const char *pref)
{
	if (assets != nullptr)
		paths::SetAssetsPath(assets);
	paths::SetBasePath(data);
	paths::SetPrefPath(pref);
	paths::SetConfigPath(pref);
	LoadCoreArchives();
	LoadGameArchives();
	ASSERT_TRUE(HaveMainData());

	gbVanilla = false;
	gbIsHellfire = false;
	gbIsSpawn = false;
	gbIsMultiplayer = false;
	gbIsHellfireSaveGame = false;
	leveltype = DTYPE_TOWN;
	giNumberOfLevels = 17;
	Players.resize(1);
	MyPlayerId = 0;
	MyPlayer = &Players[MyPlayerId];
	LoadTables();
}

std::string Describe(const Player &player)
{
	std::string s = StrCat(player._pName, " level ", static_cast<int>(player.getCharacterLevel()), " exp ", player._pExperience, " str/mag/dex/vit ",
	    player._pBaseStr, "/", player._pBaseMag, "/", player._pBaseDex, "/", player._pBaseVit, " gold ", player._pGold, ", wearing");
	for (const Item &item : player.InvBody)
		if (!item.isEmpty())
			s += StrCat(" [", item._iName, "]");
	s += ", pack";
	for (int k = 0; k < player._pNumInv; k++)
		s += StrCat(" [", player.InvList[k]._iName, "]");
	s += ", belt";
	for (const Item &item : player.SpdList)
		if (!item.isEmpty())
			s += StrCat(" [", item._iName, "]");
	return s;
}

TEST(TestHeroes, Write)
{
	const char *out = std::getenv("TEST_HEROES_OUT");
	const char *data = std::getenv("TEST_HEROES_DATA");
	if (out == nullptr || data == nullptr || std::getenv("TEST_HEROES_CONVERT_FROM") != nullptr)
		GTEST_SKIP() << "TEST_HEROES_OUT / TEST_HEROES_DATA not set (or converting)";
	const char *assets = std::getenv("TEST_HEROES_ASSETS");
	const bool hellfire = std::getenv("TEST_HEROES_HELLFIRE") != nullptr;
	SetUpGame(data, assets, out);
	if (hellfire)
		EnableHellfire(assets);

	const char *first = std::getenv("TEST_HEROES_FIRST");
	const uint32_t n = first != nullptr ? static_cast<uint32_t>(std::atoi(first)) : 1;
	MakeHero(n, HeroClass::Warrior, "DemoWarrior", "Breast Plate");
	MakeHero(n + 1, HeroClass::Rogue, "DemoRogue", "Ring Mail");
	MakeHero(n + 2, HeroClass::Sorcerer, "DemoSorcerer", "Breast Plate");
	const uint32_t count = hellfire ? 4 : 3;
	if (hellfire)
		MakeHero(n + 3, HeroClass::Monk, "DemoMonk", "Breast Plate");

	// Read back through the engine's loader
	for (uint32_t i = n; i < n + count; i++) {
		Player &player = Players[0];
		gSaveNumber = i; // LoadHeroItems reads the current save
		pfile_read_player_from_save(i, player);
		std::cout << "read single_" << i << ".sv: " << player._pName << " level " << static_cast<int>(player.getCharacterLevel()) << " exp "
		          << player._pExperience << " str " << player._pBaseStr << " spells " << std::hex << player._pMemSpells << std::dec << ", wearing";
		for (const Item &item : player.InvBody)
			if (!item.isEmpty())
				std::cout << " [" << item._iName << "]";
		std::cout << ", pack";
		for (int k = 0; k < player._pNumInv; k++)
			std::cout << " [" << player.InvList[k]._iName << "]";
		int belt = 0;
		for (const Item &item : player.SpdList)
			belt += item.isEmpty() ? 0 : 1;
		std::cout << ", belt " << belt << "\n";
		EXPECT_EQ(player.getCharacterLevel(), player.getMaxCharacterLevel());
		EXPECT_EQ(player._pNumInv, 5);
	}

	// No mod identifier is registered here, so the engine wrote .sv: the game reads Hellfire heroes as .hsv
	if (hellfire) {
		for (uint32_t i = n; i < n + count; i++) {
			std::filesystem::rename(StrCat(out, "/single_", i, ".sv"), StrCat(out, "/single_", i, ".hsv"));
			std::cout << "-> single_" << i << ".hsv\n";
		}
	}
}

TEST(TestHeroes, ConvertToHellfire)
{
	const char *from = std::getenv("TEST_HEROES_CONVERT_FROM");
	const char *out = std::getenv("TEST_HEROES_OUT");
	const char *data = std::getenv("TEST_HEROES_DATA");
	if (from == nullptr || out == nullptr || data == nullptr)
		GTEST_SKIP() << "TEST_HEROES_CONVERT_FROM / TEST_HEROES_OUT / TEST_HEROES_DATA not set";
	const char *number = std::getenv("TEST_HEROES_CONVERT");
	const uint32_t n = number != nullptr ? static_cast<uint32_t>(std::atoi(number)) : 0;
	const std::string hsv = StrCat(out, "/single_", n, ".hsv");
	ASSERT_FALSE(std::filesystem::exists(hsv)) << hsv << " exists";

	// Read the Diablo hero where it is (nothing is written there)
	SetUpGame(data, std::getenv("TEST_HEROES_ASSETS"), from);
	Player &player = Players[0];
	gSaveNumber = n;
	pfile_read_player_from_save(n, player);
	const std::string before = Describe(player);
	std::cout << "read single_" << n << ".sv (Diablo): " << before << "\n";

	// Write it as a Hellfire hero into a fresh archive
	EnableHellfire(std::getenv("TEST_HEROES_ASSETS"));
	paths::SetPrefPath(out);
	paths::SetConfigPath(out);
	MyPlayer = &player;
	pfile_write_hero(/*writeGameData=*/false);

	Player &check = Players[0];
	pfile_read_player_from_save(n, check);
	const std::string after = Describe(check);
	std::cout << "read back (Hellfire): " << after << "\n";
	EXPECT_EQ(before, after);
	std::filesystem::rename(StrCat(out, "/single_", n, ".sv"), hsv);
	std::cout << "-> " << hsv << "\n";
}

} // namespace
} // namespace devilution
