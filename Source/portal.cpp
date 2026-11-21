/**
 * @file portal.cpp
 *
 * Implementation of functionality for handling town portals.
 */
#include "portal.h"

#include "lighting.h"
#include "missiles.h"
#include "multi.h"
#include "player.h"
#include "tables/misdat.h"

namespace devilution {

static_assert(MAXPORTAL == MAX_PLRS, "one town portal per player");

/** In-game state of portals. */
Portal Portals[MAXPORTAL];

namespace {

/** Current portal number (a portal array index). */
size_t portalindex;

} // namespace

Point GetPortalTownPosition(size_t playerId)
{
	// Rows of four, two squares apart: players 1-4 on the original row (57..63, 40), players 5-8 three rows
	// further south (57..63, 43), where the squares and the arrival squares one down and right are open floor.
	const int column = static_cast<int>(playerId % 4);
	const int row = static_cast<int>(playerId / 4);
	return { 57 + (2 * column), 40 + (3 * row) };
}

void InitPortals()
{
	for (auto &portal : Portals) {
		portal.open = false;
	}
}

void SetPortalStats(int i, bool o, Point position, int lvl, dungeon_type lvltype, bool isSetLevel)
{
	Portals[i].open = o;
	Portals[i].position = position;
	Portals[i].level = lvl;
	Portals[i].ltype = lvltype;
	Portals[i].setlvl = isSetLevel;
}

void AddPortalMissile(const Player &player, Point position, bool sync)
{
	auto *missile = AddMissile({ 0, 0 }, position, Direction::South, MissileID::TownPortal, TARGET_MONSTERS, player, 0, 0, /*parent=*/nullptr, SfxID::None);
	if (missile != nullptr) {
		// Don't show portal opening animation if we sync existing portals
		if (sync)
			missile->setFrameGroup<PortalFrame>(PortalFrame::Idle);

		if (leveltype != DTYPE_TOWN)
			missile->_mlid = AddLight(missile->position.tile, 15);
	}
}

void SyncPortals()
{
	for (int i = 0; i < MAXPORTAL; i++) {
		if (!Portals[i].open)
			continue;
		const Player &player = Players[i];
		if (leveltype == DTYPE_TOWN)
			AddPortalMissile(player, GetPortalTownPosition(i), true);
		else {
			int lvl = currlevel;
			if (setlevel)
				lvl = setlvlnum;
			if (Portals[i].level == lvl && Portals[i].setlvl == setlevel)
				AddPortalMissile(player, Portals[i].position, true);
		}
	}
}

void AddPortalInTown(const Player &player)
{
	AddPortalMissile(player, GetPortalTownPosition(player.getId()), false);
}

void ActivatePortal(const Player &player, Point position, int lvl, dungeon_type dungeonType, bool isSetLevel)
{
	Portal &portal = Portals[player.getId()];
	portal.open = true;

	if (lvl != 0) {
		portal.position = position;
		portal.level = lvl;
		portal.ltype = dungeonType;
		portal.setlvl = isSetLevel;
	}
}

void DeactivatePortal(const Player &player)
{
	Portals[player.getId()].open = false;
}

bool PortalOnLevel(const Player &player)
{
	const Portal &portal = Portals[player.getId()];
	if (portal.setlvl == setlevel && portal.level == (setlevel ? static_cast<int>(setlvlnum) : currlevel))
		return true;

	return leveltype == DTYPE_TOWN;
}

void RemovePortalMissile(const Player &player)
{
	const size_t id = player.getId();
	Missiles.remove_if([id](Missile &missile) {
		if (missile._mitype == MissileID::TownPortal && missile._misource == static_cast<int>(id)) {
			dFlags[missile.position.tile.x][missile.position.tile.y] &= ~DungeonFlag::Missile;

			if (Portals[id].level != 0)
				AddUnLight(missile._mlid);

			return true;
		}
		return false;
	});
}

void SetCurrentPortal(size_t p)
{
	portalindex = p;
}

void GetPortalLevel()
{
	if (leveltype != DTYPE_TOWN) {
		setlevel = false;
		currlevel = 0;
		MyPlayer->setLevel(0);
		leveltype = DTYPE_TOWN;
		return;
	}

	if (Portals[portalindex].setlvl) {
		setlevel = true;
		setlvlnum = (_setlevels)Portals[portalindex].level;
		currlevel = Portals[portalindex].level;
		MyPlayer->setLevel(setlvlnum);
		setlvltype = leveltype = Portals[portalindex].ltype;
	} else {
		setlevel = false;
		currlevel = Portals[portalindex].level;
		MyPlayer->setLevel(currlevel);
		leveltype = Portals[portalindex].ltype;
	}

	if (portalindex == MyPlayerId) {
		NetSendCmd(true, CMD_DEACTIVATEPORTAL);
		DeactivatePortal(*MyPlayer);
	}
}

void GetPortalLvlPos()
{
	if (leveltype == DTYPE_TOWN) {
		ViewPosition = GetPortalTownPosition(portalindex) + Displacement { 1, 1 };
	} else {
		ViewPosition = Portals[portalindex].position;

		if (portalindex != MyPlayerId) {
			ViewPosition.x++;
			ViewPosition.y++;
		}
	}
}

bool PosOkPortal(int lvl, Point position)
{
	for (auto &portal : Portals) {
		if (portal.open
		    && portal.level == lvl
		    && ((portal.position == position)
		        || (portal.position == position - Displacement { 1, 1 })))
			return true;
	}
	return false;
}

} // namespace devilution
