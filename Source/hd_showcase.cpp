#include "hd_showcase.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

#include "control/control.hpp"
#include "diablo.h"
#include "engine/render/hd.hpp"
#include "engine/render/scrollrt.h"
#include "interfac.h"
#include "inv.h"
#include "cursor.h"
#include "items.h"
#include "levels/dun_tile_data.hpp"
#include "levels/gendung.h"
#include "monster.h"
#include "msg.h"
#include "player.h"
#include "tables/spelldat.h"
#include "utils/log.hpp"
#include "utils/ui_fwd.h"

namespace devilution::hd_showcase {

namespace {

struct Command {
	std::string verb;
	std::vector<std::string> args;
};

std::vector<Command> Script;
size_t NextCommand = 0;
int TicksToWait = 0;
bool ScriptRead = false;
bool LevelLoaded = false;
bool ShowHero = false;
bool KeepLight = false;
std::string Path;
std::string Palette;

const char *ScriptFile()
{
	const char *file = std::getenv("DEVILUTIONX_HD_SHOWCASE");
	return file != nullptr && *file != '\0' ? file : nullptr;
}

void ReadScript()
{
	ScriptRead = true;
	std::ifstream in(ScriptFile());
	std::string line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		std::istringstream words(line);
		Command command;
		words >> command.verb;
		if (command.verb.empty() || command.verb[0] == '#') continue;
		for (std::string word; words >> word;)
			command.args.push_back(word);
		Script.push_back(std::move(command));
	}
	LogInfo("HD showcase: {} commands from {}", Script.size(), ScriptFile());
}

/** Fully lit, nobody about: what the tiles look like, not the game. `hero` and `dim` keep the player and the game's own lighting. */
void Settle()
{
	if (!KeepLight) std::memset(dLight, 0, sizeof(dLight));
	if (!ShowHero) std::memset(dPlayer, 0, sizeof(dPlayer));
	std::memset(dMonster, 0, sizeof(dMonster));
	std::memset(dObject, 0, sizeof(dObject));
	ActiveMonsterCount = 0;
}

void WriteCamera(const std::string &name)
{
	const char *dir = std::getenv("DEVILUTIONX_HD_DUMP");
	if (dir == nullptr) return;
	const Point origin = GetScreenPosition({ 0, 0 });
	const Point east = GetScreenPosition({ 1, 0 });
	const Point south = GetScreenPosition({ 0, 1 });
	std::ofstream out(std::string(dir) + "/" + name + ".json");
	out << "{\"name\":\"" << name << "\",\"width\":" << gnScreenWidth << ",\"height\":" << gnScreenHeight
	    << ",\"panelTop\":" << GetMainPanel().position.y << ",\"view\":[" << ViewPosition.x << ',' << ViewPosition.y << "],"
	    << "\"screenOfSquare00\":[" << origin.x << ',' << origin.y << "],\"screenOfSquare10\":[" << east.x << ',' << east.y << "],\"screenOfSquare01\":["
	    << south.x << ',' << south.y << "],\"dim\":" << MAXDUNX << ",\"piece\":[";
	for (int x = 0; x < MAXDUNX; ++x)
		for (int y = 0; y < MAXDUNY; ++y)
			out << (x + y > 0 ? "," : "") << dPiece[x][y];
	out << "]}\n";
}

int Int(const Command &command, size_t i)
{
	return i < command.args.size() ? std::atoi(command.args[i].c_str()) : 0;
}

/** An item by its name: a unique item's (UniqueItems), else a base item's (AllItemsList); -> whether it was found. */
bool MakeItem(const std::string &name, bool identified, Item &item)
{
	for (size_t u = 0; u < UniqueItems.size(); ++u) {
		if (UniqueItems[u].UIName != name) continue;
		Item *spawned = SpawnUnique(static_cast<_unique_items>(u), MyPlayer->position.tile, std::nullopt, false, false);
		if (spawned == nullptr) return false;
		item = *spawned; // (the one on the floor is taken away again: the last allocated)
		dItem[spawned->position.x][spawned->position.y] = 0;
		DeleteItem(ActiveItemCount - 1);
		item._iIdentified = identified;
		return true;
	}
	for (size_t i = 0; i < AllItemsList.size(); ++i) {
		if (AllItemsList[i].iName != name) continue;
		InitializeItem(item, static_cast<_item_indexes>(i));
		item._iIdentified = identified;
		return true;
	}
	return false;
}

} // namespace

bool Active()
{
	return ScriptFile() != nullptr;
}

const std::string &MapPath()
{
	return Path;
}

const std::string &MapPalette()
{
	return Palette;
}

void AfterGameTick()
{
	if (!Active()) return;
	if (!ScriptRead) ReadScript();
	if (LevelLoaded) Settle();
	if (TicksToWait > 0) {
		--TicksToWait;
		return;
	}
	if (NextCommand >= Script.size()) {
		diablo_quit(0);
	}
	const Command &command = Script[NextCommand++];
	if (command.verb == "map" && command.args.size() >= 4) {
		Path = command.args[0];
		Palette = command.args.size() > 4 ? command.args[4] : "";
		setlvltype = static_cast<dungeon_type>(Int(command, 1));
		ViewPosition = { Int(command, 2), Int(command, 3) };
		MyPlayer->_pSLvlVisited[SL_NONE] = false;
		LogInfo("HD showcase: map {} type {} view {},{} (setlevel {} level {} before)", Path, static_cast<int>(setlvltype), ViewPosition.x, ViewPosition.y, setlevel, currlevel);
		StartNewLvl(*MyPlayer, WM_DIABSETLVL, SL_NONE);
		LogInfo("HD showcase: loaded (setlevel {} setlvlnum {} level {})", setlevel, static_cast<int>(setlvlnum), currlevel);
		LevelLoaded = true;
		Settle();
	} else if (command.verb == "view") {
		ViewPosition = { Int(command, 0), Int(command, 1) };
	} else if (command.verb == "wait") {
		TicksToWait = Int(command, 0);
	} else if (command.verb == "hero") {
		ShowHero = true;
	} else if (command.verb == "dim") {
		KeepLight = true;
	} else if (command.verb == "strip" && !command.args.empty() && command.args[0] == "pack") {
		for (Item &spot : MyPlayer->InvList)
			spot.clear();
		for (Item &spot : MyPlayer->SpdList)
			spot.clear();
		std::memset(MyPlayer->InvGrid, 0, sizeof(MyPlayer->InvGrid));
		MyPlayer->_pNumInv = 0;
		CalcPlrInv(*MyPlayer, true);
	} else if (command.verb == "strip") {
		const std::string what = command.args.empty() ? "" : command.args[0];
		for (int slot = INVLOC_HEAD; slot < NUM_INVLOC; ++slot) {
			const bool hand = slot == INVLOC_HAND_LEFT || slot == INVLOC_HAND_RIGHT;
			if (what.empty() || (what == "hands") == hand)
				MyPlayer->InvBody[slot].clear();
		}
		CalcPlrInv(*MyPlayer, true);
	} else if (command.verb == "cast" && command.args.size() >= 2) {
		const auto direction = static_cast<Direction>(Int(command, 1) & 7);
		const Point target = MyPlayer->position.tile + Displacement(direction) * 3;
		NetSendCmdLocParam3(true, CMD_SPELLXY, target, static_cast<uint16_t>(Int(command, 0)), static_cast<uint16_t>(SpellType::Spell), 0);
	} else if (command.verb == "equip" && command.args.size() >= 2) {
		static const std::pair<const char *, inv_body_loc> Slots[] = { { "head", INVLOC_HEAD }, { "chest", INVLOC_CHEST }, { "left", INVLOC_HAND_LEFT }, { "right", INVLOC_HAND_RIGHT } };
		std::string name = command.args[1];
		for (size_t i = 2; i < command.args.size(); ++i)
			name += " " + command.args[i];
		bool found = false;
		for (const auto &[word, slot] : Slots) {
			if (command.args[0] != word) continue;
			for (size_t i = 0; i < AllItemsList.size() && !found; ++i) {
				if (AllItemsList[i].iName != name) continue;
				InitializeItem(MyPlayer->InvBody[slot], static_cast<_item_indexes>(i));
				found = true;
			}
		}
		if (!found) LogError("HD showcase: nothing to equip as '{}' in '{}'", name, command.args[0]);
		CalcPlrInv(*MyPlayer, true);
	} else if (command.verb == "put" && command.args.size() >= 2) {
		// put <where> [id] <name>: an item by its name (a unique's or a base item's) at head/chest/left/right, pack, belt, hold (on the cursor)
		// or floor:<dx>:<dy> (dropped next to the hero). Unique items are unidentified unless "id" comes first.
		static const std::pair<const char *, inv_body_loc> Slots[] = { { "head", INVLOC_HEAD }, { "chest", INVLOC_CHEST }, { "left", INVLOC_HAND_LEFT }, { "right", INVLOC_HAND_RIGHT } };
		size_t first = 1;
		bool identified = false;
		if (command.args[first] == "id") {
			identified = true;
			++first;
		}
		std::string name = command.args[first];
		for (size_t i = first + 1; i < command.args.size(); ++i)
			name += " " + command.args[i];
		Item item;
		if (!MakeItem(name, identified, item)) {
			LogError("HD showcase: no item '{}'", name);
			diablo_quit(1);
			return;
		}
		const std::string &where = command.args[0];
		bool placed = false;
		for (const auto &[word, slot] : Slots) {
			if (where != word) continue;
			MyPlayer->InvBody[slot] = item;
			placed = true;
		}
		if (where == "pack") {
			placed = AutoPlaceItemInInventory(*MyPlayer, item);
		} else if (where == "belt") {
			for (Item &spot : MyPlayer->SpdList) {
				if (!spot.isEmpty()) continue;
				spot = item;
				placed = true;
				break;
			}
		} else if (where == "hold") {
			MyPlayer->HoldItem = item;
			NewCursor(item);
			placed = true;
		} else if (where.rfind("floor:", 0) == 0) {
			int dx = 0, dy = 0;
			std::sscanf(where.c_str(), "floor:%d:%d", &dx, &dy);
			PlaceItemInWorld(std::move(item), MyPlayer->position.tile + Displacement { dx, dy });
			placed = true;
		}
		if (!placed) LogError("HD showcase: can't put an item at '{}'", where);
		CalcPlrInv(*MyPlayer, true);
	} else if (command.verb == "inv") {
		invflag = true;
	} else if (command.verb == "mouse" && command.args.size() >= 2) {
		MousePosition = { Int(command, 0), Int(command, 1) };
	} else if (command.verb == "face" && !command.args.empty()) {
		StartStand(*MyPlayer, static_cast<Direction>(Int(command, 0) & 7));
	} else if (command.verb == "attack" && !command.args.empty()) {
		const auto direction = static_cast<Direction>(Int(command, 0) & 7);
		NetSendCmdLoc(MyPlayerId, true, CMD_SATTACKXY, MyPlayer->position.tile + Displacement(direction));
	} else if (command.verb == "walk" && !command.args.empty()) {
		const auto direction = static_cast<Direction>(Int(command, 0) & 7);
		NetSendCmdLoc(MyPlayerId, true, CMD_WALKXY, MyPlayer->position.tile + Displacement(direction) * std::max(Int(command, 1), 1));
	} else if (command.verb == "die") {
		const bool keep = !command.args.empty() && command.args[0] == "keep";
		StartPlayerKill(*MyPlayer, keep ? DeathReason::Unknown : DeathReason::MonsterOrTrap);
	} else if (command.verb == "shot" && !command.args.empty()) {
		WriteCamera(command.args[0]);
		hd::RequestDump(command.args[0]);
	} else if (command.verb == "mem") {
		const hd::MemoryUse use = hd::Memory();
		std::string rss = "?", hwm = "?";
		std::ifstream status("/proc/self/status");
		for (std::string line; std::getline(status, line);) {
			if (line.rfind("VmRSS:", 0) == 0) rss = line.substr(6);
			if (line.rfind("VmHWM:", 0) == 0) hwm = line.substr(6);
		}
		const auto trim = [](std::string &s) { s.erase(0, s.find_first_not_of(" \t")); };
		trim(rss);
		trim(hwm);
		LogInfo("HD showcase: mem {} hd_bytes {} hd_peak {} images {} loads {} unloads {} rss {} hwm {}", command.args.empty() ? "" : command.args[0], use.bytes, use.peak,
		    use.images, use.loads, use.unloads, rss, hwm);
	} else if (command.verb == "quit") {
		diablo_quit(0);
	} else {
		LogError("HD showcase: can't run '{}'", command.verb);
		diablo_quit(1);
	}
}

} // namespace devilution::hd_showcase
