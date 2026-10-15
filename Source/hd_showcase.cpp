#include "hd_showcase.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#include "control/control.hpp"
#include "diablo.h"
#include "engine/render/hd.hpp"
#include "engine/render/scrollrt.h"
#include "interfac.h"
#include "levels/dun_tile_data.hpp"
#include "levels/gendung.h"
#include "monster.h"
#include "player.h"
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

/** Fully lit, nobody about: what the tiles look like, not the game. */
void Settle()
{
	std::memset(dLight, 0, sizeof(dLight));
	std::memset(dPlayer, 0, sizeof(dPlayer));
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
	} else if (command.verb == "shot" && !command.args.empty()) {
		WriteCamera(command.args[0]);
		hd::RequestDump(command.args[0]);
	} else if (command.verb == "quit") {
		diablo_quit(0);
	} else {
		LogError("HD showcase: can't run '{}'", command.verb);
		diablo_quit(1);
	}
}

} // namespace devilution::hd_showcase
