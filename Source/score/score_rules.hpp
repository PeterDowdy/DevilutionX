/**
 * @file score/score_rules.hpp
 *
 * Multiplayer scoring: the rules as pure functions over a score table.
 *
 * Nothing here touches the game, the network or the screen: the module in score.cpp feeds claims in,
 * applies the keeper's awards, and asks these functions for places, leads, totals and the reveal.
 * Every function takes the game's point values (ScoreValues) from the table, never from the local ini.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace devilution::score {

/** The five ranked awards, in reveal order (smallest stake of surprise first). */
enum class Award : uint8_t {
	Identified,
	Potions,
	Gold,
	PvpKills,
	Kills,
};
constexpr size_t NumAwards = 5;
constexpr size_t NumRanks = 3;
constexpr uint8_t NoRow = 0xFF;
constexpr uint8_t NoSlot = 0xFF;
constexpr uint8_t NoFamily = 0xFF;
constexpr uint16_t NoUnique = 0xFFFF;
/** Rows are indexed by uint8_t; one is kept free for NoRow. */
constexpr size_t MaxRows = 254;

/** The game's point values: the host's [Scoring] ini section, read once when the game is created. A value of 0 switches that rule off. */
struct ScoreValues {
	uint8_t uniqueKill = 1;
	uint8_t diabloKill = 10;
	uint8_t familyLead = 1;
	uint8_t playerKillTransfer = 1;
	/** Indexed by Award, then rank (first, second, third). */
	std::array<std::array<uint8_t, NumRanks>, NumAwards> ranked { { { 5, 3, 1 }, { 5, 3, 1 }, { 5, 3, 1 }, { 5, 3, 1 }, { 5, 3, 1 } } };
	/** The same killer and victim count at most once in this many seconds for the PvP kills award. */
	uint16_t pvpRepeatSeconds = 120;

	bool operator==(const ScoreValues &other) const = default;
};

/** Number of named settings (ini keys) and the size of the values on the wire. */
constexpr size_t NumValueSettings = 20;
constexpr size_t SerializedValuesSize = 21;

struct ValueSetting {
	std::string_view key;
	int defaultValue;
	int maxValue;
};

/** The settings table: ini key, default and upper limit (lower limit 0) for each value, in wire order. */
const std::array<ValueSetting, NumValueSettings> &ValueSettings();
int GetValue(const ScoreValues &values, size_t setting);
/** Sets a value, clamped to [0, max]. */
void SetValue(ScoreValues &values, size_t setting, int value);
bool IsDefault(const ScoreValues &values);
void SerializeValues(const ScoreValues &values, uint8_t *out);
ScoreValues DeserializeValues(const uint8_t *in);

/** One player's row. Rows are keyed by hero name; a slot is reused, a row is not. */
struct ScoreRow {
	std::string name;
	/** The connected player slot, or NoSlot when the player has left. */
	uint8_t slot = NoSlot;
	int32_t banked = 0;
	uint32_t gold = 0;
	uint16_t pvpKills = 0;
	uint16_t kills = 0;
	uint16_t potions = 0;
	uint16_t identified = 0;
	/** Kills per monster family, indexed by family. */
	std::vector<uint16_t> family;
};

uint32_t AwardCount(const ScoreRow &row, Award award);

enum class EndReason : uint8_t {
	None,
	Diablo,
	Host,
};

/** The whole score state of a game. Every client holds it; only the keeper changes it on its own. */
struct ScoreTable {
	ScoreValues values;
	uint8_t familyCount = 0;
	std::vector<ScoreRow> rows;
	/** Bumped by every new keeper. Awards of an older epoch are ignored. */
	uint8_t epoch = 0;
	/** Number of awards applied in this epoch. */
	uint16_t seq = 0;
	EndReason ended = EndReason::None;
	/** Row credited with the end (Diablo's killer), or NoRow. */
	uint8_t endRow = NoRow;
	/** Keys (level << 16 | monster id) of kills already scored, sorted. */
	std::vector<uint32_t> doneMonsters;
	/** Seeds of items a player has put on the floor (second-hand), sorted. */
	std::vector<uint32_t> secondHand;
	/** Seeds of items already counted as identified, sorted. */
	std::vector<uint32_t> identifiedSeeds;
};

/** Caps that keep a serialized table under the 64 KiB of one zero-packet transfer. */
constexpr size_t MaxDoneMonsters = 5000;
constexpr size_t MaxSecondHand = 6000;
constexpr size_t MaxIdentifiedSeeds = 2000;

bool SortedContains(const std::vector<uint32_t> &set, uint32_t value);
/** Inserts into a sorted set; returns false if it was already there or the set is at its cap. */
bool SortedInsert(std::vector<uint32_t> &set, uint32_t value, size_t cap);

/** A fresh table with the given values and family count. */
ScoreTable MakeTable(const ScoreValues &values, uint8_t familyCount);
/** The row seated at a slot, or NoRow. */
uint8_t RowForSlot(const ScoreTable &table, uint8_t slot);
/** A row with that name that has no player seated, or NoRow. */
uint8_t FreeRowForName(const ScoreTable &table, std::string_view name);
uint32_t MonsterKey(uint8_t level, uint16_t monsterId);

enum class ClaimKind : uint8_t {
	/** A monster died: subject = credited slot (killer, or golem owner), or NoSlot. */
	Kill,
	/** The sender died by the killing blow of subject. */
	PlayerKill,
	/** value = gold picked up (first-hand piles, batched). */
	Gold,
	/** One potion drunk. */
	Potion,
	/** value = seed of a first-hand magic or unique item just identified. */
	Identified,
	/** value = seed of an item the sender put on the floor; applied by everyone, not decided. */
	SecondHand,
	/** The sender wants the full table (joined, or saw a gap). */
	RequestTable,
};

enum ClaimFlags : uint8_t {
	ClaimCounted = 1 << 0, // a monster of the level as generated (not spawned later)
	ClaimUnique = 1 << 1,
	ClaimDiablo = 1 << 2,
};

struct Claim {
	ClaimKind kind = ClaimKind::Kill;
	uint8_t sender = NoSlot;
	uint16_t seq = 0;
	uint8_t subject = NoSlot;
	uint8_t level = 0;
	uint16_t monsterId = 0;
	uint8_t family = NoFamily;
	uint8_t flags = 0;
	uint16_t uniqueIdx = NoUnique;
	uint32_t value = 0;
};

enum class AwardKind : uint8_t {
	AddRow,
	Seat,
	Kill,
	PlayerKill,
	Gold,
	Potion,
	Identified,
	Reject,
	Final,
};

/** A keeper decision. Applied in sequence by every client. */
struct AwardMsg {
	AwardKind kind = AwardKind::Reject;
	uint8_t row = NoRow;
	/** PlayerKill: the victim. */
	uint8_t row2 = NoRow;
	uint8_t family = NoFamily;
	/** Kill: ClaimFlags. PlayerKill: 1 if it counts for the PvP kills award. */
	uint8_t flags = 0;
	/** AddRow / Seat: the slot. */
	uint8_t slot = NoSlot;
	/** Kill: banked points to row. PlayerKill: banked points moved from row2 to row. */
	int16_t banked = 0;
	/** Kill: monster key. Gold: amount. Identified: seed. Final: EndReason. */
	uint32_t value = 0;
	uint16_t uniqueIdx = NoUnique;
	/** The claim this answers (sender slot + claim seq), or NoSlot. */
	uint8_t claimSlot = NoSlot;
	uint16_t claimSeq = 0;
	/** AddRow: the hero name. */
	std::string name;
};

/** Remembers when each killer last killed each victim (rows), for the PvP repeat window. Keeper-local. */
struct PvpMemory {
	struct Entry {
		uint8_t killer;
		uint8_t victim;
		uint32_t timeMs;
	};
	std::vector<Entry> entries;
};

/**
 * @brief The keeper's decision on one claim: zero or more awards (a Diablo kill gives the kill and the final).
 * Every decided claim gets at least one award (Reject if nothing changes), so the other clients can drop it from their pending list.
 */
std::vector<AwardMsg> DecideClaim(const ScoreTable &table, const Claim &claim, uint32_t nowMs, PvpMemory &pvp);

/** Applies one award; returns false if it does not fit the table (the caller then asks for the full table). Does not touch seq. */
bool ApplyAward(ScoreTable &table, const AwardMsg &award);

/** One place of a ranked award. */
struct RankEntry {
	/** 0 = first, 1 = second, 2 = third. */
	uint8_t place = 0;
	/** The row(s) at this place; more than one means tied (nobody paid, the ranks are used up). */
	std::vector<uint8_t> rows;
	uint32_t count = 0;
	uint8_t points = 0;
	[[nodiscard]] bool tied() const { return rows.size() > 1; }
};

/** Places for one award: at most three entries; a tie of n uses up n places. Zero counts never rank. */
std::vector<RankEntry> RankAward(const ScoreTable &table, Award award);

struct FamilyLead {
	uint8_t family = NoFamily;
	/** The leader, or NoRow when tied. */
	uint8_t row = NoRow;
	uint16_t count = 0;
	uint8_t points = 0;
	[[nodiscard]] bool tied() const { return row == NoRow; }
};

/** Leads of the families that have kills, in the given order (dungeon order). */
std::vector<FamilyLead> FamilyLeads(const ScoreTable &table, const std::vector<uint8_t> &familyOrder);

struct RowTotal {
	int32_t banked = 0;
	int32_t families = 0;
	int32_t awards = 0;
	int32_t total = 0;
	/** 0-based place; equal totals share the place (1, 1, 3). */
	uint8_t place = 0;
};

/** Where one row stands in one ranked award during play: a place, or none. Never a count. */
struct LivePlace {
	/** 0 = first, 1 = second, 2 = third; NoPlace when the row has no place. */
	uint8_t place = 0xFF;
	bool tied = false;
	[[nodiscard]] bool placed() const { return place != 0xFF; }
};

/** One row of the live board: what everyone may see during play (Peter 2026-11-20): places, plus the banked points. */
struct LiveRow {
	uint8_t row = NoRow;
	/** 0-based overall place (totals as if the game ended now); equal totals share it. */
	uint8_t overallPlace = 0;
	/** Banked points (uniques, Diablo, player-kill transfers) are public: their events are announced in chat. */
	int32_t banked = 0;
	std::array<LivePlace, NumAwards> awards {};
	bool left = false;
};

/** The live board, rows in overall order. Holds no counter (kills, gold, potions, identified, PvP kills) by construction. */
std::vector<LiveRow> LiveBoard(const ScoreTable &table);

/** Live family leads: per family with kills (in the given order), the rows at the top; one row = the leader, more = tied. */
struct LiveFamily {
	uint8_t family = NoFamily;
	std::vector<uint8_t> rows;
	[[nodiscard]] bool tied() const { return rows.size() > 1; }
};
std::vector<LiveFamily> LiveFamilies(const ScoreTable &table, const std::vector<uint8_t> &familyOrder);

/** Totals as if the game ended now. */
std::vector<RowTotal> Totals(const ScoreTable &table);
/** Row indices by total (highest first), ties in row order. */
std::vector<uint8_t> OverallOrder(const std::vector<RowTotal> &totals);

enum class RevealStepKind : uint8_t {
	/** Why it ended and the banked standings. */
	Banked,
	/** A slice of the family leads. */
	Families,
	/** One ranked award. */
	Award,
	/** The final table. */
	Final,
};

struct RevealStep {
	RevealStepKind kind = RevealStepKind::Banked;
	/** Families: index into the FamilyLeads list and how many lines. */
	size_t first = 0;
	size_t count = 0;
	Award award = Award::Identified;
};

constexpr size_t FamiliesPerRevealStep = 6;

/** The stepped reveal: banked, families six a step, the five awards (identified first), final table. Rules worth 0 are left out. */
std::vector<RevealStep> BuildRevealSteps(const ScoreTable &table, const std::vector<uint8_t> &familyOrder);
/** Each row's total counting only what the first `shown` steps revealed (the totals column counting up). */
std::vector<int32_t> RunningTotals(const ScoreTable &table, const std::vector<uint8_t> &familyOrder, const std::vector<RevealStep> &steps, size_t shown);

/** Wire format of the full table (little endian). */
std::vector<uint8_t> SerializeTable(const ScoreTable &table);
std::optional<ScoreTable> DeserializeTable(const uint8_t *data, size_t size);

/** A stable text dump of the table (rows, counters, totals, values), for the test harness checkpoints. */
std::string DescribeTable(const ScoreTable &table);

} // namespace devilution::score
