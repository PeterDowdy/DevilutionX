/**
 * @file score/score.cpp
 *
 * Multiplayer scoring: the game side (claims, the score keeper, awards, the table on the wire, chat lines).
 *
 * One score keeper decides (the lowest-numbered connected player that holds the table; on TCP the host).
 * Clients send small claims to everyone; the keeper turns each into numbered awards that every client applies
 * in order; the full table travels to joiners, after a gap, from a new keeper, and at the end.
 * The rules themselves are pure functions in score_rules.cpp.
 *
 * Nothing here is saved, nothing is added to Player / Monster / Item, and nothing draws from the game's
 * random generator, so single player, saves and demos are untouched; with scoring off no claim, award or
 * table is ever sent.
 */
#include "score/score.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#ifdef USE_SDL3
#include <SDL3/SDL_timer.h>
#else
#include <SDL.h>
#endif

#include "engine/demomode.h"
#include "items.h"
#include "levels/gendung.h"
#include "monster.h"
#include "msg.h"
#include "multi.h"
#include "options.h"
#include "player.h"
#include "plrmsg.h"
#include "score/score_rules.hpp"
#include "tables/monstdat.h"
#include "utils/endian_swap.hpp"
#include "utils/format.hpp"
#include "utils/language.h"
#include "utils/log.hpp"
#include "utils/str_case.hpp"
#include "utils/str_cat.hpp"

namespace devilution {

namespace {

using score::Award;
using score::AwardKind;
using score::AwardMsg;
using score::Claim;
using score::ClaimKind;
using score::EndReason;
using score::NoFamily;
using score::NoRow;
using score::NoSlot;
using score::NoUnique;
using score::ScoreTable;
using score::ScoreValues;

#pragma pack(push, 1)
struct TCmdScoreClaim {
	_cmd_id bCmd;
	uint8_t kind;
	uint16_t seq;
	uint8_t subject;
	uint8_t level;
	uint16_t monsterId;
	uint8_t family;
	uint8_t flags;
	uint16_t uniqueIdx;
	uint32_t value;
};

struct TCmdScoreAward {
	_cmd_id bCmd;
	uint8_t kind;
	uint8_t epoch;
	uint16_t seq;
	uint8_t row;
	uint8_t row2;
	uint8_t family;
	uint8_t flags;
	uint8_t slot;
	int16_t banked;
	uint32_t value;
	uint16_t uniqueIdx;
	uint8_t claimSlot;
	uint16_t claimSeq;
	/** AddRow: the hero name follows (this many bytes). */
	uint8_t nameLength;
};
#pragma pack(pop)

/** Gold claims are batched to one a second. */
constexpr uint32_t GoldBatchMs = 1000;
/** A client asks for the full table at most this often. */
constexpr uint32_t TableRequestMs = 2000;
/** Claims seen but not answered are kept this long, for a new keeper to pick up. */
constexpr uint32_t PendingClaimMs = 30000;
/** How many first-hand pickups a client remembers (for identify). */
constexpr size_t MaxFreshPickups = 4000;

/** One monster family: a sprite set, with Peter's three merges. */
struct FamilyDef {
	std::string folder;
	std::string name;
	int minLevel = 99;
};

/** Merged sprite sets (folder -> the folder it joins). */
constexpr std::pair<std::string_view, std::string_view> FamilyMerges[] = {
	{ "falsword", "falspear" },
	{ "skelbow", "skelaxe" },
	{ "skelsd", "skelaxe" },
	{ "goatbow", "goatmace" },
};
/** Bosses with a sprite set of their own are outside every family (Peter); the golem is never a victim. */
constexpr std::string_view FamilylessFolders[] = { "sking", "fatc", "diablo", "golem" };
/** Display names for the families of a Diablo game; other sets fall back to their first monster's name. */
constexpr std::pair<std::string_view, const char *> FamilyNames[] = {
	{ "zombie", N_("Zombies") },
	{ "falspear", N_("Fallen") },
	{ "skelaxe", N_("Skeletons") },
	{ "scav", N_("Scavengers") },
	{ "sneak", N_("Hidden") },
	{ "bat", N_("Bats") },
	{ "goatmace", N_("Goatmen") },
	{ "fat", N_("Overlords") },
	{ "gargoyle", N_("Gargoyles") },
	{ "acid", N_("Acid beasts") },
	{ "rhino", N_("Horned demons") },
	{ "magma", N_("Magma demons") },
	{ "thin", N_("Storm demons") },
	{ "mega", N_("Balrogs") },
	{ "snake", N_("Vipers") },
	{ "black", N_("Knights") },
	{ "succ", N_("Succubi") },
	{ "mage", N_("Mages") },
};

std::vector<FamilyDef> Families;
/** Family of each monster type (index = _monster_id), or NoFamily. */
std::vector<uint8_t> TypeFamily;
/** Family indices in dungeon order (lowest first level first). */
std::vector<uint8_t> FamilyOrder;

struct PendingClaim {
	Claim claim;
	uint32_t seenAt;
};

struct ScoreState {
	bool haveTable = false;
	ScoreTable table;
	bool wasKeeper = false;
	uint16_t nextClaimSeq = 0;
	std::vector<PendingClaim> pending;
	score::PvpMemory pvp;
	uint32_t lastTableRequest = 0;
	uint32_t goldPending = 0;
	uint32_t goldLastSent = 0;
	/** Second-hand marks seen before the table arrived (a joiner). */
	std::vector<uint32_t> earlyMarks;
	/** Seeds of items the local player picked up first-hand in this game (identify counts only these). */
	std::vector<uint32_t> freshPickups;
	/** Full-table transfers being reassembled, per sending slot. */
	std::array<std::vector<uint8_t>, MAX_PLRS> chunks;
	/** Diagnostics for the harness. */
	uint32_t claimsSent = 0;
	uint32_t rejected = 0;
	uint32_t tablesReceived = 0;
	/** First game tick without a table (joiner). */
	uint32_t startedAt = 0;
};

ScoreState State;
/** Which player's damage is being applied to the local player right now. */
uint8_t DamageSource = NoSlot;

std::string ToLower(std::string_view text)
{
	return AsciiStrToLower(text);
}

std::string FolderOf(const MonsterData &data)
{
	std::string_view path = data.spritePath();
	const size_t sep = path.find_first_of("\\/");
	if (sep != std::string_view::npos)
		path = path.substr(0, sep);
	return ToLower(path);
}

void BuildFamilies()
{
	Families.clear();
	TypeFamily.assign(MonstersData.size(), NoFamily);
	for (size_t type = 0; type < MonstersData.size(); type++) {
		const MonsterData &data = MonstersData[type];
		std::string folder = FolderOf(data);
		for (const auto &[from, to] : FamilyMerges) {
			if (folder == from)
				folder = std::string(to);
		}
		if (std::find(std::begin(FamilylessFolders), std::end(FamilylessFolders), folder) != std::end(FamilylessFolders))
			continue;
		auto it = std::find_if(Families.begin(), Families.end(), [&](const FamilyDef &f) { return f.folder == folder; });
		if (it == Families.end()) {
			if (Families.size() >= NoFamily)
				continue;
			FamilyDef def;
			def.folder = folder;
			def.name = data.name;
			for (const auto &[key, name] : FamilyNames) {
				if (folder == key)
					def.name = name;
			}
			Families.push_back(std::move(def));
			it = Families.end() - 1;
		}
		if (data.availability != MonsterAvailability::Never && data.minDunLvl > 0)
			it->minLevel = std::min<int>(it->minLevel, data.minDunLvl);
		TypeFamily[type] = static_cast<uint8_t>(it - Families.begin());
	}
	FamilyOrder.resize(Families.size());
	for (size_t i = 0; i < FamilyOrder.size(); i++)
		FamilyOrder[i] = static_cast<uint8_t>(i);
	std::stable_sort(FamilyOrder.begin(), FamilyOrder.end(), [](uint8_t a, uint8_t b) { return Families[a].minLevel < Families[b].minLevel; });
}

ScoreValues ValuesFromOptions()
{
	ScoreValues values;
	const auto &entries = GetOptions().Scoring.values;
	for (size_t i = 0; i < score::NumValueSettings && i < entries.size(); i++)
		score::SetValue(values, i, *entries[i]);
	return values;
}

uint32_t Now()
{
	return static_cast<uint32_t>(SDL_GetTicks());
}

bool SlotConnected(uint8_t slot)
{
	return slot < Players.size() && (slot == MyPlayerId || Players[slot].plractive);
}

/** The keeper: the lowest connected slot that has a row in the table. */
uint8_t KeeperSlot()
{
	if (!State.haveTable)
		return NoSlot;
	uint8_t best = NoSlot;
	for (const score::ScoreRow &row : State.table.rows) {
		if (row.slot != NoSlot && row.slot < best && SlotConnected(row.slot))
			best = row.slot;
	}
	return best;
}

bool AmKeeper()
{
	return KeeperSlot() == MyPlayerId;
}

std::string_view RowName(uint8_t row)
{
	if (row < State.table.rows.size())
		return State.table.rows[row].name;
	return _("Nobody");
}

void SendClaim(Claim claim)
{
	TCmdScoreClaim message {};
	message.bCmd = CMD_SCORE_CLAIM;
	message.kind = static_cast<uint8_t>(claim.kind);
	message.seq = Swap16LE(State.nextClaimSeq++);
	message.subject = claim.subject;
	message.level = claim.level;
	message.monsterId = Swap16LE(claim.monsterId);
	message.family = claim.family;
	message.flags = claim.flags;
	message.uniqueIdx = Swap16LE(claim.uniqueIdx);
	message.value = Swap32LE(claim.value);
	NetSendHiPri(MyPlayerId, reinterpret_cast<std::byte *>(&message), sizeof(message));
	State.claimsSent++;
}

void SendAward(const AwardMsg &award)
{
	std::byte buffer[sizeof(TCmdScoreAward) + PlayerNameLength];
	TCmdScoreAward message {};
	message.bCmd = CMD_SCORE_AWARD;
	message.kind = static_cast<uint8_t>(award.kind);
	message.epoch = State.table.epoch;
	message.seq = Swap16LE(State.table.seq);
	message.row = award.row;
	message.row2 = award.row2;
	message.family = award.family;
	message.flags = award.flags;
	message.slot = award.slot;
	message.banked = static_cast<int16_t>(Swap16LE(static_cast<uint16_t>(award.banked)));
	message.value = Swap32LE(award.value);
	message.uniqueIdx = Swap16LE(award.uniqueIdx);
	message.claimSlot = award.claimSlot;
	message.claimSeq = Swap16LE(award.claimSeq);
	const size_t nameLength = award.kind == AwardKind::AddRow ? std::min<size_t>(award.name.size(), PlayerNameLength) : 0;
	message.nameLength = static_cast<uint8_t>(nameLength);
	memcpy(buffer, &message, sizeof(message));
	memcpy(buffer + sizeof(message), award.name.data(), nameLength);
	NetSendHiPri(MyPlayerId, buffer, sizeof(message) + nameLength);
}

void SendTableTo(uint8_t pnum)
{
	if (pnum == MyPlayerId || !SlotConnected(pnum))
		return;
	const std::vector<uint8_t> table = score::SerializeTable(State.table);
	std::vector<std::byte> data(4 + table.size());
	const uint32_t size = static_cast<uint32_t>(table.size());
	for (int i = 0; i < 4; i++)
		data[i] = static_cast<std::byte>(size >> (8 * i));
	memcpy(data.data() + 4, table.data(), table.size());
	if (data.size() > 0xFFFF) {
		LogError("Score table too large to send ({} bytes)", data.size());
		return;
	}
	multi_send_zero_packet(pnum, CMD_SCORE_TABLE, data.data(), data.size());
}

void BroadcastTable()
{
	for (size_t slot = 0; slot < Players.size(); slot++)
		SendTableTo(static_cast<uint8_t>(slot));
}

void RequestTable()
{
	const uint32_t now = Now();
	if (State.lastTableRequest != 0 && now - State.lastTableRequest < TableRequestMs)
		return;
	State.lastTableRequest = now;
	Claim claim;
	claim.kind = ClaimKind::RequestTable;
	SendClaim(claim);
}

void AnnounceFinal()
{
	const ScoreTable &table = State.table;
	if (table.ended == EndReason::Host) {
		EventPlrMsg(_("The host ended the game. The scores are final."));
	} else if (table.endRow != NoRow) {
		EventPlrMsg(FormatRuntime(_("{:s} killed Diablo. The scores are final."), RowName(table.endRow)));
	} else {
		EventPlrMsg(_("Diablo is dead. The scores are final."));
	}
	const std::vector<score::RowTotal> totals = score::Totals(table);
	std::vector<std::string_view> winners;
	int32_t best = 0;
	for (size_t i = 0; i < totals.size(); i++) {
		if (totals[i].place == 0) {
			winners.push_back(table.rows[i].name);
			best = totals[i].total;
		}
	}
	if (winners.size() == 1) {
		EventPlrMsg(FormatRuntime(ngettext("{:s} wins with {:d} point.", "{:s} wins with {:d} points.", best), winners[0], best));
	} else if (!winners.empty()) {
		std::string names;
		for (const std::string_view name : winners)
			StrAppend(names, names.empty() ? "" : ", ", name);
		EventPlrMsg(FormatRuntime(ngettext("Tied for first: {:s} with {:d} point.", "Tied for first: {:s} with {:d} points.", best), names, best));
	}
	EventPlrMsg(_("Type /score or open the score board to see the scores."));
}

void ForgetPending(uint8_t claimSlot, uint16_t claimSeq)
{
	std::erase_if(State.pending, [&](const PendingClaim &p) { return p.claim.sender == claimSlot && p.claim.seq == claimSeq; });
}

/** Chat line for a banked event (every client prints it once, when it applies the award). */
void AnnounceAward(const AwardMsg &award)
{
	switch (award.kind) {
	case AwardKind::Kill:
		if (award.banked <= 0 || award.row == NoRow)
			return;
		if ((award.flags & score::ClaimDiablo) != 0) {
			EventPlrMsg(FormatRuntime(_("{:s} killed {:s}: +{:d}"), RowName(award.row), pgettext("monster", MonstersData[MT_DIABLO].name), award.banked));
		} else if (award.uniqueIdx < UniqueMonstersData.size()) {
			EventPlrMsg(FormatRuntime(_("{:s} killed {:s}: +{:d}"), RowName(award.row), pgettext("monster", UniqueMonstersData[award.uniqueIdx].mName), award.banked));
		}
		break;
	case AwardKind::PlayerKill:
		if (award.banked <= 0)
			return;
		EventPlrMsg(FormatRuntime(ngettext("{:s} killed {:s} and took {:d} point", "{:s} killed {:s} and took {:d} points", award.banked), RowName(award.row), RowName(award.row2), award.banked));
		break;
	default:
		break;
	}
}

void OnScoringClosed();

/** Applies an award to the local table (keeper at decision time, the others on receipt). */
bool ApplyLocal(const AwardMsg &award)
{
	const bool wasOpen = State.table.ended == EndReason::None;
	if (!score::ApplyAward(State.table, award))
		return false;
	State.table.seq++;
	if (award.claimSlot != NoSlot)
		ForgetPending(award.claimSlot, award.claimSeq);
	if (award.kind == AwardKind::Reject)
		State.rejected++;
	AnnounceAward(award);
	if (wasOpen && State.table.ended != EndReason::None)
		OnScoringClosed();
	return true;
}

/** Keeper: decide, apply and broadcast. */
void KeeperDecide(const AwardMsg &award)
{
	if (!ApplyLocal(award)) {
		LogError("Score keeper could not apply its own award {}", static_cast<int>(award.kind));
		return;
	}
	SendAward(award);
}

void KeeperHandleClaim(const Claim &claim)
{
	const std::vector<AwardMsg> awards = score::DecideClaim(State.table, claim, Now(), State.pvp);
	bool final = false;
	for (const AwardMsg &award : awards) {
		KeeperDecide(award);
		final = final || award.kind == AwardKind::Final;
	}
	if (final)
		BroadcastTable();
}

void KeeperSeat(const Player &player)
{
	const uint8_t slot = player.getId();
	const std::string_view name = player._pName;
	const uint8_t seated = score::RowForSlot(State.table, slot);
	if (seated != NoRow && State.table.rows[seated].name == name)
		return;
	AwardMsg award;
	award.slot = slot;
	const uint8_t free = score::FreeRowForName(State.table, name);
	if (free != NoRow) {
		award.kind = AwardKind::Seat;
		award.row = free;
	} else {
		if (State.table.rows.size() >= score::MaxRows)
			return;
		award.kind = AwardKind::AddRow;
		award.row = static_cast<uint8_t>(State.table.rows.size());
		award.name = std::string(name);
	}
	KeeperDecide(award);
}

void AcceptTable(ScoreTable &&table)
{
	State.tablesReceived++;
	if (State.haveTable) {
		const ScoreTable &cur = State.table;
		if (table.epoch < cur.epoch || (table.epoch == cur.epoch && table.seq < cur.seq))
			return;
	}
	const bool wasOpen = !State.haveTable || State.table.ended == EndReason::None;
	State.table = std::move(table);
	State.haveTable = true;
	State.wasKeeper = AmKeeper();
	for (const uint32_t seed : State.earlyMarks)
		score::SortedInsert(State.table.secondHand, seed, score::MaxSecondHand);
	State.earlyMarks.clear();
	if (State.table.familyCount != Families.size())
		LogError("Score table has {} families, this client {}", State.table.familyCount, Families.size());
	if (wasOpen && State.table.ended != EndReason::None)
		OnScoringClosed();
}

void OnScoringClosed()
{
	AnnounceFinal();
}

} // namespace

bool ScoringActive()
{
	return gbIsMultiplayer && sgGameInitInfo.scoring != 0 && !demo::IsRunning();
}

bool ScoringOpen()
{
	return ScoringActive() && (!State.haveTable || State.table.ended == EndReason::None);
}

void ScoreInitGameInfo(GameData &gameData)
{
	const bool on = gbIsMultiplayer && *GetOptions().Gameplay.scoring;
	gameData.scoring = on ? 1 : 0;
	gameData.scoringCustom = on && !score::IsDefault(ValuesFromOptions()) ? 1 : 0;
}

void ScoreOnGameStart(bool isCreator)
{
	State = {};
	DamageSource = NoSlot;
	if (!ScoringActive())
		return;
	BuildFamilies();
	if (!isCreator)
		return;
	State.table = score::MakeTable(ValuesFromOptions(), static_cast<uint8_t>(Families.size()));
	State.table.epoch = 1;
	State.haveTable = true;
	AwardMsg self;
	self.kind = AwardKind::AddRow;
	self.row = 0;
	self.slot = MyPlayerId;
	self.name = MyPlayer->_pName;
	ApplyLocal(self);
	State.wasKeeper = true;
	LogInfo("Scoring on ({} families{})", Families.size(), score::IsDefault(State.table.values) ? "" : ", custom values");
}

void ScoreOnGameEnd()
{
	State = {};
	ScoreBoardFlag = false;
}

void ScoreOnPlayerActive(const Player &player)
{
	if (!ScoringActive() || !AmKeeper() || &player == MyPlayer)
		return;
	KeeperSeat(player);
	SendTableTo(player.getId());
}

void ScoreOnPlayerLeft(const Player &player)
{
	if (!ScoringActive() || !State.haveTable)
		return;
	const uint8_t row = score::RowForSlot(State.table, player.getId());
	if (row != NoRow)
		State.table.rows[row].slot = NoSlot;
}

void ScoreTick()
{
	if (!ScoringActive())
		return;
	const uint32_t now = Now();
	if (!State.haveTable) {
		// A joiner gets the table from the keeper when it arrives on its first level; ask again if that got lost.
		if (State.startedAt == 0)
			State.startedAt = now;
		else if (now - State.startedAt > 5000)
			RequestTable();
		return;
	}
	if (AmKeeper()) {
		if (!State.wasKeeper) {
			// Handover: a new epoch, the table to everyone, then the claims nobody answered.
			State.wasKeeper = true;
			State.table.epoch++;
			State.table.seq = 0;
			LogInfo("Score keeper is now player {} (epoch {})", MyPlayerId, State.table.epoch);
			BroadcastTable();
			std::vector<PendingClaim> pending = std::move(State.pending);
			State.pending.clear();
			for (const PendingClaim &p : pending)
				KeeperHandleClaim(p.claim);
		}
	} else {
		State.wasKeeper = false;
	}
	std::erase_if(State.pending, [&](const PendingClaim &p) { return now - p.seenAt > PendingClaimMs; });
	if (State.goldPending > 0 && now - State.goldLastSent >= GoldBatchMs && ScoringOpen()) {
		Claim claim;
		claim.kind = ClaimKind::Gold;
		claim.value = State.goldPending;
		SendClaim(claim);
		State.goldPending = 0;
		State.goldLastSent = now;
	}
}

void ScoreOnMonsterDeath(const Monster &monster, uint8_t creditedSlot)
{
	if (!ScoringOpen() || monster.isPlayerMinion())
		return;
	const Player &myPlayer = *MyPlayer;
	const bool isDiablo = monster.type().type == MT_DIABLO;
	if (creditedSlot == NoSlot && !(isDiablo && myPlayer.isLevelOwnedByLocalClient()))
		return;
	const uint8_t level = GetLevelForMultiplayer(myPlayer);
	const auto monsterId = static_cast<uint16_t>(monster.getId());
	Claim claim;
	claim.kind = ClaimKind::Kill;
	claim.subject = creditedSlot;
	claim.level = level;
	claim.monsterId = monsterId;
	if (!DeltaIsSpawnedMonster(level, monsterId))
		claim.flags |= score::ClaimCounted;
	if (monster.isUnique()) {
		claim.flags |= score::ClaimUnique;
		claim.uniqueIdx = static_cast<uint16_t>(monster.uniqueType);
	}
	if (isDiablo)
		claim.flags |= score::ClaimDiablo;
	if (claim.flags == 0)
		return;
	const auto type = static_cast<size_t>(monster.type().type);
	claim.family = type < TypeFamily.size() ? TypeFamily[type] : NoFamily;
	SendClaim(claim);
}

ScoreDamageSource::ScoreDamageSource(uint8_t slot)
    : previous_(DamageSource)
{
	DamageSource = slot;
}

ScoreDamageSource::~ScoreDamageSource()
{
	DamageSource = previous_;
}

void ScoreOnLocalPlayerDeath(DeathReason deathReason)
{
	if (!ScoringOpen() || deathReason != DeathReason::Player)
		return;
	if (DamageSource == NoSlot || DamageSource == MyPlayerId || DamageSource >= Players.size())
		return;
	Claim claim;
	claim.kind = ClaimKind::PlayerKill;
	claim.subject = DamageSource;
	SendClaim(claim);
}

bool IsSecondHand(uint32_t seed)
{
	if (!State.haveTable)
		return std::find(State.earlyMarks.begin(), State.earlyMarks.end(), seed) != State.earlyMarks.end();
	return score::SortedContains(State.table.secondHand, seed);
}

void ScoreOnGoldPickedUp(const Item &pile, uint32_t amount)
{
	// Gold no player has held before, each pile once: piles a player put down are second-hand.
	if (!ScoringOpen() || amount == 0 || IsSecondHand(static_cast<uint32_t>(pile._iSeed)))
		return;
	State.goldPending += amount;
}

void ScoreOnItemPickedUp(const Item &item)
{
	if (!ScoringOpen() || IsSecondHand(static_cast<uint32_t>(item._iSeed)))
		return;
	score::SortedInsert(State.freshPickups, static_cast<uint32_t>(item._iSeed), MaxFreshPickups);
}

void ScoreOnPlayerDrop(const Item &item)
{
	if (!ScoringOpen() || item.isEmpty())
		return;
	Claim claim;
	claim.kind = ClaimKind::SecondHand;
	claim.value = static_cast<uint32_t>(item._iSeed);
	SendClaim(claim);
}

void ScoreOnPotionDrunk(int miscId, bool lifeOrManaBelowMax)
{
	// Healing, mana and rejuvenation potions (small and full) drunk in the dungeon while not at full life or mana.
	// Elixirs and arena potions are not potions here.
	if (!ScoringOpen() || leveltype == DTYPE_TOWN || !lifeOrManaBelowMax)
		return;
	switch (static_cast<item_misc_id>(miscId)) {
	case IMISC_HEAL:
	case IMISC_FULLHEAL:
	case IMISC_MANA:
	case IMISC_FULLMANA:
	case IMISC_REJUV:
	case IMISC_FULLREJUV:
		break;
	default:
		return;
	}
	Claim claim;
	claim.kind = ClaimKind::Potion;
	SendClaim(claim);
}

void ScoreOnIdentified(const Item &item)
{
	// Magic or unique items this player picked up first-hand in this game; the keeper counts each item once.
	if (!ScoringOpen() || item._iMagical == ITEM_QUALITY_NORMAL || item._iIdentified)
		return;
	const auto seed = static_cast<uint32_t>(item._iSeed);
	if (!score::SortedContains(State.freshPickups, seed))
		return;
	Claim claim;
	claim.kind = ClaimKind::Identified;
	claim.value = seed;
	SendClaim(claim);
}

size_t ScoreOnClaimCmd(uint8_t pnum, const TCmd *cmd, size_t maxCmdSize, bool apply)
{
	if (maxCmdSize < sizeof(TCmdScoreClaim))
		return 0;
	if (!apply || !ScoringActive())
		return sizeof(TCmdScoreClaim);
	const auto &message = *reinterpret_cast<const TCmdScoreClaim *>(cmd);
	Claim claim;
	claim.kind = static_cast<ClaimKind>(message.kind);
	claim.sender = pnum;
	claim.seq = Swap16LE(message.seq);
	claim.subject = message.subject;
	claim.level = message.level;
	claim.monsterId = Swap16LE(message.monsterId);
	claim.family = message.family;
	claim.flags = message.flags;
	claim.uniqueIdx = Swap16LE(message.uniqueIdx);
	claim.value = Swap32LE(message.value);
	if (message.kind > static_cast<uint8_t>(ClaimKind::RequestTable))
		return sizeof(TCmdScoreClaim);

	switch (claim.kind) {
	case ClaimKind::SecondHand:
		if (State.haveTable)
			score::SortedInsert(State.table.secondHand, claim.value, score::MaxSecondHand);
		else
			State.earlyMarks.push_back(claim.value);
		break;
	case ClaimKind::RequestTable:
		if (AmKeeper())
			SendTableTo(pnum);
		break;
	default:
		if (AmKeeper())
			KeeperHandleClaim(claim);
		else
			State.pending.push_back({ claim, Now() });
		break;
	}
	return sizeof(TCmdScoreClaim);
}

size_t ScoreOnAwardCmd(uint8_t pnum, const TCmd *cmd, size_t maxCmdSize, bool apply)
{
	if (maxCmdSize < sizeof(TCmdScoreAward))
		return 0;
	const auto &message = *reinterpret_cast<const TCmdScoreAward *>(cmd);
	const size_t size = sizeof(TCmdScoreAward) + message.nameLength;
	if (maxCmdSize < size || message.nameLength > PlayerNameLength)
		return 0;
	if (!apply || !ScoringActive() || !State.haveTable || pnum == MyPlayerId)
		return size;
	ScoreTable &table = State.table;
	const uint16_t seq = Swap16LE(message.seq);
	if (message.epoch < table.epoch)
		return size;
	if (message.epoch > table.epoch || seq > table.seq + 1) {
		RequestTable();
		return size;
	}
	if (seq != table.seq + 1)
		return size;
	if (message.kind > static_cast<uint8_t>(AwardKind::Final))
		return size;
	AwardMsg award;
	award.kind = static_cast<AwardKind>(message.kind);
	award.row = message.row;
	award.row2 = message.row2;
	award.family = message.family;
	award.flags = message.flags;
	award.slot = message.slot;
	award.banked = static_cast<int16_t>(Swap16LE(static_cast<uint16_t>(message.banked)));
	award.value = Swap32LE(message.value);
	award.uniqueIdx = Swap16LE(message.uniqueIdx);
	award.claimSlot = message.claimSlot;
	award.claimSeq = Swap16LE(message.claimSeq);
	award.name.assign(reinterpret_cast<const char *>(cmd) + sizeof(TCmdScoreAward), message.nameLength);
	if (!ApplyLocal(award))
		RequestTable();
	return size;
}

size_t ScoreOnTableCmd(uint8_t pnum, const TCmd *cmd, size_t maxCmdSize)
{
	const auto &header = *reinterpret_cast<const TCmdPlrInfoHdr *>(cmd);
	if (maxCmdSize < sizeof(header))
		return 0;
	const uint16_t wBytes = Swap16LE(header.wBytes);
	const uint16_t wOffset = Swap16LE(header.wOffset);
	const size_t size = sizeof(header) + wBytes;
	if (maxCmdSize < size)
		return 0;
	if (!ScoringActive() || pnum >= State.chunks.size() || pnum == MyPlayerId)
		return size;
	std::vector<uint8_t> &buffer = State.chunks[pnum];
	if (wOffset == 0)
		buffer.clear();
	if (wOffset != buffer.size()) {
		buffer.clear();
		return size;
	}
	const auto *bytes = reinterpret_cast<const uint8_t *>(&header + 1);
	buffer.insert(buffer.end(), bytes, bytes + wBytes);
	if (buffer.size() < 4)
		return size;
	const uint32_t total = buffer[0] | (buffer[1] << 8) | (buffer[2] << 16) | (static_cast<uint32_t>(buffer[3]) << 24);
	if (buffer.size() < 4 + static_cast<size_t>(total))
		return size;
	std::optional<ScoreTable> table = score::DeserializeTable(buffer.data() + 4, total);
	buffer.clear();
	if (!table) {
		LogError("Invalid score table from player {}", pnum);
		return size;
	}
	AcceptTable(std::move(*table));
	return size;
}

void ScoreChatCommand()
{
	if (!ScoringActive())
		return;
	if (!State.haveTable) {
		EventPlrMsg(_("The score table has not arrived yet."));
		return;
	}
	const ScoreTable &table = State.table;
	const std::vector<score::RowTotal> totals = score::Totals(table);
	std::string line = std::string(_("Standings:"));
	for (const uint8_t row : score::OverallOrder(totals))
		StrAppend(line, " ", totals[row].place + 1, ". ", table.rows[row].name);
	EventPlrMsg(line);
}

bool ScoreCanEndGame()
{
	return ScoringOpen() && State.haveTable && AmKeeper();
}

void ScoreEndGameByHost()
{
	if (!ScoreCanEndGame())
		return;
	AwardMsg award;
	award.kind = AwardKind::Final;
	award.value = static_cast<uint32_t>(EndReason::Host);
	KeeperDecide(award);
	BroadcastTable();
}

std::string ScoreDescribeForHarness()
{
	if (!ScoringActive())
		return "off";
	if (!State.haveTable)
		return "notable";
	return StrCat("epoch=", State.table.epoch, " seq=", State.table.seq, " keeper=", KeeperSlot(), " ", score::DescribeTable(State.table));
}

} // namespace devilution
