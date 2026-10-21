/**
 * Writes demo heroes for the HD remaster: one per class, maximum level and stats, every spell at level 15 and a kit that shows every
 * player animation (unarmed, shield, sword, sword + shield, mace, mace + shield, axe, bow, staff; light, medium and heavy armour: medium and
 * heavy are worn or carried, light is no chest armour).
 *
 * Opt-in: skipped unless TEST_HEROES_OUT (save directory to write) and TEST_HEROES_DATA (folder of diabdat.mpq) are set.
 *   TEST_HEROES_OUT=DIR TEST_HEROES_DATA=DIR TEST_HEROES_ASSETS=BUILD/assets/ ./testheroes_test
 * Saves are single_<N>.sv with N = TEST_HEROES_FIRST (default 1) and up.
 */
#include <cstdlib>
#include <cstring>
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

TEST(TestHeroes, Write)
{
	const char *out = std::getenv("TEST_HEROES_OUT");
	const char *data = std::getenv("TEST_HEROES_DATA");
	if (out == nullptr || data == nullptr)
		GTEST_SKIP() << "TEST_HEROES_OUT / TEST_HEROES_DATA not set";
	if (const char *assets = std::getenv("TEST_HEROES_ASSETS"))
		paths::SetAssetsPath(assets);
	paths::SetBasePath(data);
	paths::SetPrefPath(out);
	paths::SetConfigPath(out);
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
	LoadSpellData();
	LoadPlayerDataFiles();
	LoadMonsterData();
	LoadItemData();

	const char *first = std::getenv("TEST_HEROES_FIRST");
	const uint32_t n = first != nullptr ? static_cast<uint32_t>(std::atoi(first)) : 1;
	MakeHero(n, HeroClass::Warrior, "DemoWarrior", "Breast Plate");
	MakeHero(n + 1, HeroClass::Rogue, "DemoRogue", "Ring Mail");
	MakeHero(n + 2, HeroClass::Sorcerer, "DemoSorcerer", "Breast Plate");

	// Read back through the engine's loader
	for (uint32_t i = n; i < n + 3; i++) {
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
}

} // namespace
} // namespace devilution
