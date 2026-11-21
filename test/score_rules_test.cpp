#include <gtest/gtest.h>

#include "score/score_rules.hpp"

namespace devilution::score {
namespace {

constexpr uint8_t Families = 4;

ScoreTable TableWith(std::initializer_list<const char *> names, const ScoreValues &values = {})
{
	ScoreTable table = MakeTable(values, Families);
	uint8_t slot = 0;
	for (const char *name : names) {
		AwardMsg add;
		add.kind = AwardKind::AddRow;
		add.row = static_cast<uint8_t>(table.rows.size());
		add.slot = slot++;
		add.name = name;
		EXPECT_TRUE(ApplyAward(table, add));
	}
	return table;
}

/** Decide and apply, as the keeper does. */
std::vector<AwardMsg> RunClaim(ScoreTable &table, const Claim &claim, uint32_t nowMs = 0)
{
	static PvpMemory pvp;
	std::vector<AwardMsg> awards = DecideClaim(table, claim, nowMs, pvp);
	for (const AwardMsg &award : awards) {
		EXPECT_TRUE(ApplyAward(table, award));
		table.seq++;
	}
	return awards;
}

Claim KillClaim(uint8_t slot, uint16_t monsterId, uint8_t family, uint8_t flags = ClaimCounted, uint8_t level = 1)
{
	Claim claim;
	claim.kind = ClaimKind::Kill;
	claim.sender = slot;
	claim.subject = slot;
	claim.level = level;
	claim.monsterId = monsterId;
	claim.family = family;
	claim.flags = flags;
	return claim;
}

Claim SimpleClaim(ClaimKind kind, uint8_t slot, uint32_t value = 0)
{
	Claim claim;
	claim.kind = kind;
	claim.sender = slot;
	claim.value = value;
	return claim;
}

TEST(ScoreRules, ValueSettingsDefaultsAndRoundTrip)
{
	const ScoreValues defaults;
	ASSERT_EQ(ValueSettings().size(), NumValueSettings);
	for (size_t i = 0; i < NumValueSettings; i++)
		EXPECT_EQ(GetValue(defaults, i), ValueSettings()[i].defaultValue) << ValueSettings()[i].key;
	EXPECT_TRUE(IsDefault(defaults));

	ScoreValues changed;
	for (size_t i = 0; i < NumValueSettings; i++)
		SetValue(changed, i, static_cast<int>(i * 7 + 2));
	SetValue(changed, 19, 9999); // clamped to 3600
	EXPECT_EQ(changed.pvpRepeatSeconds, 3600);
	SetValue(changed, 0, -5); // clamped to 0
	EXPECT_EQ(changed.uniqueKill, 0);
	EXPECT_FALSE(IsDefault(changed));
	uint8_t wire[SerializedValuesSize];
	SerializeValues(changed, wire);
	EXPECT_EQ(DeserializeValues(wire), changed);
	EXPECT_EQ(GetValue(changed, 4), changed.ranked[static_cast<size_t>(Award::Gold)][0]);
	EXPECT_EQ(GetValue(changed, 18), changed.ranked[static_cast<size_t>(Award::Identified)][2]);
}

TEST(ScoreRules, UniqueKillOnlyOnceAndKillingBlowOnly)
{
	ScoreTable table = TableWith({ "Anna", "Bo" });
	auto awards = RunClaim(table, KillClaim(0, 40, 1, ClaimCounted | ClaimUnique));
	ASSERT_EQ(awards.size(), 1U);
	EXPECT_EQ(awards[0].kind, AwardKind::Kill);
	EXPECT_EQ(table.rows[0].banked, 1);
	EXPECT_EQ(table.rows[0].kills, 1);
	EXPECT_EQ(table.rows[0].family[1], 1);
	// Same-instant double kill: the second claim for the same monster is refused.
	awards = RunClaim(table, KillClaim(1, 40, 1, ClaimCounted | ClaimUnique));
	ASSERT_EQ(awards.size(), 1U);
	EXPECT_EQ(awards[0].kind, AwardKind::Reject);
	EXPECT_EQ(table.rows[1].banked, 0);
	EXPECT_EQ(table.rows[1].kills, 0);
	// The same monster number on another level is another monster.
	RunClaim(table, KillClaim(1, 40, 1, ClaimCounted, 2));
	EXPECT_EQ(table.rows[1].kills, 1);
}

TEST(ScoreRules, ClaimsOnlyForYourself)
{
	ScoreTable table = TableWith({ "Anna", "Bo" });
	Claim claim = KillClaim(0, 3, 0);
	claim.subject = 1;
	EXPECT_EQ(RunClaim(table, claim)[0].kind, AwardKind::Reject);
	// A claim from a slot with no seat is refused.
	EXPECT_EQ(RunClaim(table, KillClaim(5, 3, 0))[0].kind, AwardKind::Reject);
}

TEST(ScoreRules, SpawnedMonstersDoNotCount)
{
	ScoreTable table = TableWith({ "Anna" });
	EXPECT_EQ(RunClaim(table, KillClaim(0, 7, 0, 0))[0].kind, AwardKind::Reject);
	EXPECT_EQ(table.rows[0].kills, 0);
}

TEST(ScoreRules, DiabloPaysAndEndsTheGame)
{
	ScoreTable table = TableWith({ "Anna", "Bo" });
	auto awards = RunClaim(table, KillClaim(1, 0, NoFamily, ClaimCounted | ClaimDiablo, 16));
	ASSERT_EQ(awards.size(), 2U);
	EXPECT_EQ(awards[1].kind, AwardKind::Final);
	EXPECT_EQ(table.rows[1].banked, 10);
	EXPECT_EQ(table.ended, EndReason::Diablo);
	EXPECT_EQ(table.endRow, 1);
	// After the end nothing scores.
	EXPECT_EQ(RunClaim(table, KillClaim(0, 9, 0, ClaimCounted | ClaimUnique))[0].kind, AwardKind::Reject);
	EXPECT_EQ(table.rows[0].banked, 0);
}

TEST(ScoreRules, DiabloKilledByNobodyStillEnds)
{
	ScoreTable table = TableWith({ "Anna" });
	Claim claim = KillClaim(0, 0, NoFamily, ClaimCounted | ClaimDiablo, 16);
	claim.subject = NoSlot;
	auto awards = RunClaim(table, claim);
	ASSERT_EQ(awards.size(), 2U);
	EXPECT_EQ(table.rows[0].banked, 0);
	EXPECT_EQ(table.ended, EndReason::Diablo);
	EXPECT_EQ(table.endRow, NoRow);
}

TEST(ScoreRules, ChangedValuesApply)
{
	ScoreValues values;
	values.uniqueKill = 3;
	values.diabloKill = 0;
	ScoreTable table = TableWith({ "Anna" }, values);
	RunClaim(table, KillClaim(0, 1, 0, ClaimCounted | ClaimUnique));
	EXPECT_EQ(table.rows[0].banked, 3);
	RunClaim(table, KillClaim(0, 2, NoFamily, ClaimCounted | ClaimDiablo, 16));
	EXPECT_EQ(table.rows[0].banked, 3);
}

TEST(ScoreRules, PlayerKillMovesBankedPoints)
{
	ScoreTable table = TableWith({ "Anna", "Bo" });
	Claim death = SimpleClaim(ClaimKind::PlayerKill, 0);
	death.subject = 1;
	// Victim has 0 banked: nothing moves, but the PvP kill counts.
	auto awards = RunClaim(table, death, 1000);
	EXPECT_EQ(awards[0].banked, 0);
	EXPECT_EQ(table.rows[1].pvpKills, 1);
	EXPECT_EQ(table.rows[0].banked, 0);

	RunClaim(table, KillClaim(0, 1, 0, ClaimCounted | ClaimUnique));
	RunClaim(table, KillClaim(0, 2, 0, ClaimCounted | ClaimUnique));
	EXPECT_EQ(table.rows[0].banked, 2);
	// Within the repeat window: the point moves, the PvP kill does not count again.
	awards = RunClaim(table, death, 30000);
	EXPECT_EQ(awards[0].banked, 1);
	EXPECT_EQ(table.rows[0].banked, 1);
	EXPECT_EQ(table.rows[1].banked, 1);
	EXPECT_EQ(table.rows[1].pvpKills, 1);
	// After the window it counts again.
	RunClaim(table, death, 1000 + 121000);
	EXPECT_EQ(table.rows[1].pvpKills, 2);
	EXPECT_EQ(table.rows[1].banked, 2);
	EXPECT_EQ(table.rows[0].banked, 0);
	// Killing yourself is not a player kill.
	Claim self = death;
	self.subject = 0;
	EXPECT_EQ(RunClaim(table, self)[0].kind, AwardKind::Reject);
}

TEST(ScoreRules, CountersAndIdentifyOnce)
{
	ScoreTable table = TableWith({ "Anna" });
	RunClaim(table, SimpleClaim(ClaimKind::Gold, 0, 250));
	RunClaim(table, SimpleClaim(ClaimKind::Gold, 0, 50));
	RunClaim(table, SimpleClaim(ClaimKind::Potion, 0));
	RunClaim(table, SimpleClaim(ClaimKind::Identified, 0, 0xABCD));
	EXPECT_EQ(RunClaim(table, SimpleClaim(ClaimKind::Identified, 0, 0xABCD))[0].kind, AwardKind::Reject);
	EXPECT_EQ(table.rows[0].gold, 300U);
	EXPECT_EQ(table.rows[0].potions, 1);
	EXPECT_EQ(table.rows[0].identified, 1);
}

void SetCounts(ScoreTable &table, Award award, std::initializer_list<uint32_t> counts)
{
	size_t i = 0;
	for (const uint32_t count : counts) {
		ScoreRow &row = table.rows[i++];
		switch (award) {
		case Award::Gold:
			row.gold = count;
			break;
		case Award::Kills:
			row.kills = static_cast<uint16_t>(count);
			break;
		case Award::PvpKills:
			row.pvpKills = static_cast<uint16_t>(count);
			break;
		case Award::Potions:
			row.potions = static_cast<uint16_t>(count);
			break;
		case Award::Identified:
			row.identified = static_cast<uint16_t>(count);
			break;
		}
	}
}

TEST(ScoreRules, RankedAwardsPlainOrder)
{
	ScoreTable table = TableWith({ "A", "B", "C", "D" });
	SetCounts(table, Award::Gold, { 100, 400, 300, 200 });
	const auto ranks = RankAward(table, Award::Gold);
	ASSERT_EQ(ranks.size(), 3U);
	EXPECT_EQ(ranks[0].rows, std::vector<uint8_t> { 1 });
	EXPECT_EQ(ranks[0].points, 5);
	EXPECT_EQ(ranks[1].rows, std::vector<uint8_t> { 2 });
	EXPECT_EQ(ranks[1].points, 3);
	EXPECT_EQ(ranks[2].rows, std::vector<uint8_t> { 3 });
	EXPECT_EQ(ranks[2].points, 1);
}

TEST(ScoreRules, RankedTieUsesUpRanks)
{
	ScoreTable table = TableWith({ "A", "B", "C", "D" });
	// Two tied for most: no 5 and no 3; the next player is third and gets 1.
	SetCounts(table, Award::Kills, { 50, 50, 20, 10 });
	auto ranks = RankAward(table, Award::Kills);
	ASSERT_EQ(ranks.size(), 2U);
	EXPECT_TRUE(ranks[0].tied());
	EXPECT_EQ(ranks[0].points, 0);
	EXPECT_EQ(ranks[1].place, 2);
	EXPECT_EQ(ranks[1].rows, std::vector<uint8_t> { 2 });
	EXPECT_EQ(ranks[1].points, 1);
	// A tie for second uses up second and third.
	SetCounts(table, Award::Kills, { 50, 20, 20, 10 });
	ranks = RankAward(table, Award::Kills);
	ASSERT_EQ(ranks.size(), 2U);
	EXPECT_EQ(ranks[0].points, 5);
	EXPECT_TRUE(ranks[1].tied());
	EXPECT_EQ(ranks[1].points, 0);
	// Three tied at the top: nobody is paid at all.
	SetCounts(table, Award::Kills, { 9, 9, 9, 1 });
	ranks = RankAward(table, Award::Kills);
	ASSERT_EQ(ranks.size(), 1U);
	EXPECT_EQ(ranks[0].rows.size(), 3U);
}

TEST(ScoreRules, ZeroNeverRanksAndFewPlayers)
{
	ScoreTable table = TableWith({ "A", "B", "C" });
	SetCounts(table, Award::Potions, { 0, 4, 0 });
	auto ranks = RankAward(table, Award::Potions);
	ASSERT_EQ(ranks.size(), 1U);
	EXPECT_EQ(ranks[0].rows, std::vector<uint8_t> { 1 });
	// Two players: first and second only.
	ScoreTable two = TableWith({ "A", "B" });
	SetCounts(two, Award::Potions, { 3, 4 });
	ranks = RankAward(two, Award::Potions);
	ASSERT_EQ(ranks.size(), 2U);
	EXPECT_EQ(ranks[0].points, 5);
	EXPECT_EQ(ranks[1].points, 3);
}

TEST(ScoreRules, FamilyLeadsAndTies)
{
	ScoreTable table = TableWith({ "A", "B" });
	table.rows[0].family = { 5, 2, 0, 0 };
	table.rows[1].family = { 3, 2, 0, 1 };
	const auto leads = FamilyLeads(table, { 3, 2, 1, 0 });
	ASSERT_EQ(leads.size(), 3U); // family 2 has no kills: no leader, no point
	EXPECT_EQ(leads[0].family, 3);
	EXPECT_EQ(leads[0].row, 1);
	EXPECT_EQ(leads[0].points, 1);
	EXPECT_EQ(leads[1].family, 1);
	EXPECT_TRUE(leads[1].tied());
	EXPECT_EQ(leads[1].points, 0);
	EXPECT_EQ(leads[2].row, 0);
}

TEST(ScoreRules, TotalsAndPlaces)
{
	ScoreTable table = TableWith({ "A", "B", "C" });
	table.rows[0].banked = 4;
	table.rows[1].banked = 2;
	table.rows[2].banked = 0;
	table.rows[1].family = { 1, 0, 0, 0 }; // B leads family 0: +1
	SetCounts(table, Award::Gold, { 0, 0, 10 }); // C first in gold: +5
	const auto totals = Totals(table);
	EXPECT_EQ(totals[0].total, 4);
	EXPECT_EQ(totals[1].total, 3);
	EXPECT_EQ(totals[1].families, 1);
	EXPECT_EQ(totals[2].total, 5);
	EXPECT_EQ(totals[2].awards, 5);
	EXPECT_EQ(totals[2].place, 0);
	EXPECT_EQ(totals[0].place, 1);
	EXPECT_EQ(totals[1].place, 2);
	EXPECT_EQ(OverallOrder(totals), (std::vector<uint8_t> { 2, 0, 1 }));
	// Equal totals share the place.
	table.rows[0].banked = 5;
	const auto tied = Totals(table);
	EXPECT_EQ(tied[0].place, 0);
	EXPECT_EQ(tied[2].place, 0);
	EXPECT_EQ(tied[1].place, 2);
}

TEST(ScoreRules, LiveBoardShowsBankedAndPlacesOnly)
{
	ScoreTable table = TableWith({ "A", "B", "C" });
	table.rows[0].banked = 2;
	table.rows[1].banked = 7;
	table.rows[2].banked = -1;
	table.rows[2].slot = NoSlot; // C left
	SetCounts(table, Award::Gold, { 300, 300, 50 });
	SetCounts(table, Award::Kills, { 0, 4, 9 });
	table.rows[0].family = { 3, 0, 0, 0 };
	table.rows[1].family = { 3, 1, 0, 0 };
	const auto live = LiveBoard(table);
	ASSERT_EQ(live.size(), 3U);
	// B: banked 7 + kills second 3 + family 1 lead 1 = 11; C: -1 + gold third 1 + kills first 5 = 5; A: 2.
	EXPECT_EQ(live[0].row, 1);
	EXPECT_EQ(live[0].banked, 7);
	EXPECT_EQ(live[0].overallPlace, 0);
	EXPECT_EQ(live[1].row, 2);
	EXPECT_EQ(live[1].banked, -1);
	EXPECT_TRUE(live[1].left);
	EXPECT_EQ(live[2].row, 0);
	EXPECT_EQ(live[2].banked, 2);
	// Gold: A and B tied for first (no points, two ranks used up), C third.
	const size_t gold = static_cast<size_t>(Award::Gold);
	EXPECT_EQ(live[2].awards[gold].place, 0);
	EXPECT_TRUE(live[2].awards[gold].tied);
	EXPECT_EQ(live[0].awards[gold].place, 0);
	EXPECT_EQ(live[1].awards[gold].place, 2);
	EXPECT_FALSE(live[1].awards[gold].tied);
	// Kills: a zero count has no place.
	EXPECT_FALSE(live[2].awards[static_cast<size_t>(Award::Kills)].placed());
	EXPECT_FALSE(live[0].awards[static_cast<size_t>(Award::Potions)].placed());
	// Families: tied rows listed, a single leader alone; families without kills are not listed.
	const auto families = LiveFamilies(table, { 0, 1, 2, 3 });
	ASSERT_EQ(families.size(), 2U);
	EXPECT_TRUE(families[0].tied());
	EXPECT_EQ(families[0].rows, (std::vector<uint8_t> { 0, 1 }));
	EXPECT_EQ(families[1].rows, std::vector<uint8_t> { 1 });
}

TEST(ScoreRules, RevealStepsAndRunningTotals)
{
	ScoreTable table = TableWith({ "A", "B" });
	table.rows[0].banked = 2;
	table.rows[1].banked = 1;
	// Seven families with kills would need two family steps; with four families, one.
	table.rows[0].family = { 1, 1, 0, 0 };
	table.rows[1].family = { 0, 0, 3, 0 };
	SetCounts(table, Award::Identified, { 2, 1 });
	const std::vector<uint8_t> order { 0, 1, 2, 3 };
	auto steps = BuildRevealSteps(table, order);
	ASSERT_EQ(steps.size(), 1U + 1U + NumAwards + 1U);
	EXPECT_EQ(steps.front().kind, RevealStepKind::Banked);
	EXPECT_EQ(steps[1].kind, RevealStepKind::Families);
	EXPECT_EQ(steps[1].count, 3U);
	EXPECT_EQ(steps[2].kind, RevealStepKind::Award);
	EXPECT_EQ(steps[2].award, Award::Identified);
	EXPECT_EQ(steps[6].award, Award::Kills);
	EXPECT_EQ(steps.back().kind, RevealStepKind::Final);

	EXPECT_EQ(RunningTotals(table, order, steps, 0), (std::vector<int32_t> { 0, 0 }));
	EXPECT_EQ(RunningTotals(table, order, steps, 1), (std::vector<int32_t> { 2, 1 }));
	EXPECT_EQ(RunningTotals(table, order, steps, 2), (std::vector<int32_t> { 4, 2 }));
	EXPECT_EQ(RunningTotals(table, order, steps, 3), (std::vector<int32_t> { 9, 5 }));
	const auto totals = Totals(table);
	const auto last = RunningTotals(table, order, steps, steps.size());
	EXPECT_EQ(last[0], totals[0].total);
	EXPECT_EQ(last[1], totals[1].total);

	// Rules worth 0 are left out of the reveal.
	table.values.familyLead = 0;
	table.values.ranked[static_cast<size_t>(Award::Gold)] = { 0, 0, 0 };
	steps = BuildRevealSteps(table, order);
	EXPECT_EQ(steps.size(), 1U + (NumAwards - 1) + 1U);
	for (const RevealStep &step : steps) {
		EXPECT_NE(step.kind, RevealStepKind::Families);
		if (step.kind == RevealStepKind::Award) {
			EXPECT_NE(step.award, Award::Gold);
		}
	}
}

TEST(ScoreRules, FamiliesSixAStep)
{
	ScoreTable table = MakeTable({}, 14);
	AwardMsg add;
	add.kind = AwardKind::AddRow;
	add.row = 0;
	add.slot = 0;
	add.name = "A";
	ASSERT_TRUE(ApplyAward(table, add));
	std::vector<uint8_t> order;
	for (uint8_t f = 0; f < 14; f++) {
		table.rows[0].family[f] = 1;
		order.push_back(f);
	}
	const auto steps = BuildRevealSteps(table, order);
	ASSERT_GE(steps.size(), 4U);
	EXPECT_EQ(steps[1].count, 6U);
	EXPECT_EQ(steps[2].count, 6U);
	EXPECT_EQ(steps[3].count, 2U);
	EXPECT_EQ(steps[3].first, 12U);
}

TEST(ScoreRules, RowsByHeroNameAndSeats)
{
	ScoreTable table = TableWith({ "Anna", "Bo" });
	// Bo leaves; his row stays and is found by name for a rejoin.
	table.rows[1].slot = NoSlot;
	EXPECT_EQ(FreeRowForName(table, "Bo"), 1);
	EXPECT_EQ(FreeRowForName(table, "Anna"), NoRow); // seated, not free
	AwardMsg seat;
	seat.kind = AwardKind::Seat;
	seat.row = 1;
	seat.slot = 3;
	ASSERT_TRUE(ApplyAward(table, seat));
	EXPECT_EQ(RowForSlot(table, 3), 1);
	// A new player in Anna's old slot takes the slot from her row.
	AwardMsg add;
	add.kind = AwardKind::AddRow;
	add.row = 2;
	add.slot = 0;
	add.name = "Cy";
	ASSERT_TRUE(ApplyAward(table, add));
	EXPECT_EQ(table.rows[0].slot, NoSlot);
	EXPECT_EQ(RowForSlot(table, 0), 2);
	// An AddRow out of order does not fit.
	add.row = 7;
	EXPECT_FALSE(ApplyAward(table, add));
}

TEST(ScoreRules, SerializeRoundTrip)
{
	ScoreValues values;
	values.ranked[static_cast<size_t>(Award::Gold)][0] = 9;
	ScoreTable table = TableWith({ "Anna", "Bo" }, values);
	RunClaim(table, KillClaim(0, 4, 2, ClaimCounted | ClaimUnique));
	RunClaim(table, SimpleClaim(ClaimKind::Gold, 1, 77));
	RunClaim(table, SimpleClaim(ClaimKind::Identified, 1, 1234));
	SortedInsert(table.secondHand, 42, MaxSecondHand);
	table.epoch = 3;
	table.rows[1].slot = NoSlot;
	const auto bytes = SerializeTable(table);
	const auto back = DeserializeTable(bytes.data(), bytes.size());
	ASSERT_TRUE(back.has_value());
	EXPECT_EQ(DescribeTable(*back), DescribeTable(table));
	EXPECT_EQ(back->epoch, 3);
	EXPECT_EQ(back->seq, table.seq);
	EXPECT_EQ(back->values, table.values);
	// Truncated data is refused.
	EXPECT_FALSE(DeserializeTable(bytes.data(), bytes.size() - 1).has_value());
}

} // namespace
} // namespace devilution::score
