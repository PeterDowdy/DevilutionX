/**
 * @file score/score_rules.cpp
 *
 * Multiplayer scoring rules (pure functions). See score_rules.hpp.
 */
#include "score/score_rules.hpp"

#include <algorithm>
#include <limits>

namespace devilution::score {

namespace {

constexpr size_t FirstRankedSetting = 4;
constexpr size_t PvpRepeatSetting = 19;
/** Ranked settings are grouped by award in this order in the settings table (the plan's order). */
constexpr std::array<Award, NumAwards> SettingAwardOrder { Award::Gold, Award::PvpKills, Award::Kills, Award::Potions, Award::Identified };

const std::array<ValueSetting, NumValueSettings> Settings { {
	{ "UniqueKill", 1, 255 },
	{ "DiabloKill", 10, 255 },
	{ "FamilyLead", 1, 255 },
	{ "PlayerKillTransfer", 1, 255 },
	{ "GoldFirst", 5, 255 },
	{ "GoldSecond", 3, 255 },
	{ "GoldThird", 1, 255 },
	{ "PvpKillsFirst", 5, 255 },
	{ "PvpKillsSecond", 3, 255 },
	{ "PvpKillsThird", 1, 255 },
	{ "KillsFirst", 5, 255 },
	{ "KillsSecond", 3, 255 },
	{ "KillsThird", 1, 255 },
	{ "PotionsFirst", 5, 255 },
	{ "PotionsSecond", 3, 255 },
	{ "PotionsThird", 1, 255 },
	{ "IdentifiedFirst", 5, 255 },
	{ "IdentifiedSecond", 3, 255 },
	{ "IdentifiedThird", 1, 255 },
	{ "PvpRepeatSeconds", 120, 3600 },
} };

template <typename T>
void SaturatingAdd(T &counter, uint32_t amount)
{
	const uint64_t sum = static_cast<uint64_t>(counter) + amount;
	counter = static_cast<T>(std::min<uint64_t>(sum, std::numeric_limits<T>::max()));
}

class Writer {
public:
	std::vector<uint8_t> bytes;

	void u8(uint8_t v) { bytes.push_back(v); }
	void u16(uint16_t v)
	{
		u8(static_cast<uint8_t>(v));
		u8(static_cast<uint8_t>(v >> 8));
	}
	void u32(uint32_t v)
	{
		u16(static_cast<uint16_t>(v));
		u16(static_cast<uint16_t>(v >> 16));
	}
	void set(const std::vector<uint32_t> &values)
	{
		u32(static_cast<uint32_t>(values.size()));
		for (const uint32_t v : values)
			u32(v);
	}
};

class Reader {
public:
	Reader(const uint8_t *data, size_t size)
	    : data_(data)
	    , size_(size)
	{
	}

	bool ok = true;

	uint8_t u8()
	{
		if (pos_ + 1 > size_) {
			ok = false;
			return 0;
		}
		return data_[pos_++];
	}
	uint16_t u16()
	{
		const uint16_t lo = u8();
		return static_cast<uint16_t>(lo | (u8() << 8));
	}
	uint32_t u32()
	{
		const uint32_t lo = u16();
		return lo | (static_cast<uint32_t>(u16()) << 16);
	}
	const uint8_t *take(size_t n)
	{
		if (pos_ + n > size_) {
			ok = false;
			return nullptr;
		}
		const uint8_t *p = data_ + pos_;
		pos_ += n;
		return p;
	}
	bool set(std::vector<uint32_t> &values, size_t cap)
	{
		const uint32_t n = u32();
		if (!ok || n > cap || n > (size_ - pos_) / 4)
			return ok = false;
		values.resize(n);
		for (uint32_t &v : values)
			v = u32();
		return ok && std::is_sorted(values.begin(), values.end());
	}

private:
	const uint8_t *data_;
	size_t size_;
	size_t pos_ = 0;
};

constexpr uint8_t TableFormatVersion = 1;
constexpr size_t MaxNameLength = 32;

bool ValidRow(const ScoreTable &table, uint8_t row)
{
	return row < table.rows.size();
}

AwardMsg Answer(const Claim &claim, AwardKind kind)
{
	AwardMsg award;
	award.kind = kind;
	award.claimSlot = claim.sender;
	award.claimSeq = claim.seq;
	return award;
}

uint8_t RankPoints(const ScoreValues &values, Award award, size_t place)
{
	return values.ranked[static_cast<size_t>(award)][place];
}

bool AwardHasPoints(const ScoreValues &values, Award award)
{
	for (size_t place = 0; place < NumRanks; place++) {
		if (RankPoints(values, award, place) != 0)
			return true;
	}
	return false;
}

} // namespace

const std::array<ValueSetting, NumValueSettings> &ValueSettings()
{
	return Settings;
}

int GetValue(const ScoreValues &values, size_t setting)
{
	switch (setting) {
	case 0:
		return values.uniqueKill;
	case 1:
		return values.diabloKill;
	case 2:
		return values.familyLead;
	case 3:
		return values.playerKillTransfer;
	case PvpRepeatSetting:
		return values.pvpRepeatSeconds;
	default:
		if (setting >= FirstRankedSetting && setting < PvpRepeatSetting) {
			const size_t i = setting - FirstRankedSetting;
			return values.ranked[static_cast<size_t>(SettingAwardOrder[i / NumRanks])][i % NumRanks];
		}
		return 0;
	}
}

void SetValue(ScoreValues &values, size_t setting, int value)
{
	if (setting >= NumValueSettings)
		return;
	value = std::clamp(value, 0, Settings[setting].maxValue);
	switch (setting) {
	case 0:
		values.uniqueKill = static_cast<uint8_t>(value);
		break;
	case 1:
		values.diabloKill = static_cast<uint8_t>(value);
		break;
	case 2:
		values.familyLead = static_cast<uint8_t>(value);
		break;
	case 3:
		values.playerKillTransfer = static_cast<uint8_t>(value);
		break;
	case PvpRepeatSetting:
		values.pvpRepeatSeconds = static_cast<uint16_t>(value);
		break;
	default: {
		const size_t i = setting - FirstRankedSetting;
		values.ranked[static_cast<size_t>(SettingAwardOrder[i / NumRanks])][i % NumRanks] = static_cast<uint8_t>(value);
	} break;
	}
}

bool IsDefault(const ScoreValues &values)
{
	return values == ScoreValues {};
}

void SerializeValues(const ScoreValues &values, uint8_t *out)
{
	for (size_t i = 0; i < PvpRepeatSetting; i++)
		out[i] = static_cast<uint8_t>(GetValue(values, i));
	out[PvpRepeatSetting] = static_cast<uint8_t>(values.pvpRepeatSeconds);
	out[PvpRepeatSetting + 1] = static_cast<uint8_t>(values.pvpRepeatSeconds >> 8);
}

ScoreValues DeserializeValues(const uint8_t *in)
{
	ScoreValues values;
	for (size_t i = 0; i < PvpRepeatSetting; i++)
		SetValue(values, i, in[i]);
	SetValue(values, PvpRepeatSetting, in[PvpRepeatSetting] | (in[PvpRepeatSetting + 1] << 8));
	return values;
}

uint32_t AwardCount(const ScoreRow &row, Award award)
{
	switch (award) {
	case Award::Identified:
		return row.identified;
	case Award::Potions:
		return row.potions;
	case Award::Gold:
		return row.gold;
	case Award::PvpKills:
		return row.pvpKills;
	case Award::Kills:
		return row.kills;
	}
	return 0;
}

bool SortedContains(const std::vector<uint32_t> &set, uint32_t value)
{
	return std::binary_search(set.begin(), set.end(), value);
}

bool SortedInsert(std::vector<uint32_t> &set, uint32_t value, size_t cap)
{
	const auto it = std::lower_bound(set.begin(), set.end(), value);
	if (it != set.end() && *it == value)
		return false;
	if (set.size() >= cap)
		return false;
	set.insert(it, value);
	return true;
}

ScoreTable MakeTable(const ScoreValues &values, uint8_t familyCount)
{
	ScoreTable table;
	table.values = values;
	table.familyCount = familyCount;
	return table;
}

uint8_t RowForSlot(const ScoreTable &table, uint8_t slot)
{
	if (slot == NoSlot)
		return NoRow;
	for (size_t i = 0; i < table.rows.size(); i++) {
		if (table.rows[i].slot == slot)
			return static_cast<uint8_t>(i);
	}
	return NoRow;
}

uint8_t FreeRowForName(const ScoreTable &table, std::string_view name)
{
	for (size_t i = 0; i < table.rows.size(); i++) {
		if (table.rows[i].slot == NoSlot && table.rows[i].name == name)
			return static_cast<uint8_t>(i);
	}
	return NoRow;
}

uint32_t MonsterKey(uint8_t level, uint16_t monsterId)
{
	return (static_cast<uint32_t>(level) << 16) | monsterId;
}

std::vector<AwardMsg> DecideClaim(const ScoreTable &table, const Claim &claim, uint32_t nowMs, PvpMemory &pvp)
{
	const uint8_t senderRow = RowForSlot(table, claim.sender);
	if (table.ended != EndReason::None || senderRow == NoRow)
		return { Answer(claim, AwardKind::Reject) };

	switch (claim.kind) {
	case ClaimKind::Kill: {
		if (claim.subject != NoSlot && claim.subject != claim.sender)
			return { Answer(claim, AwardKind::Reject) };
		if ((claim.flags & (ClaimCounted | ClaimUnique | ClaimDiablo)) == 0)
			return { Answer(claim, AwardKind::Reject) };
		const uint32_t key = MonsterKey(claim.level, claim.monsterId);
		if (SortedContains(table.doneMonsters, key))
			return { Answer(claim, AwardKind::Reject) };
		AwardMsg kill = Answer(claim, AwardKind::Kill);
		kill.row = claim.subject == NoSlot ? NoRow : senderRow;
		kill.flags = claim.flags & (ClaimCounted | ClaimUnique | ClaimDiablo);
		kill.family = (claim.flags & ClaimCounted) != 0 ? claim.family : NoFamily;
		kill.value = key;
		kill.uniqueIdx = claim.uniqueIdx;
		if (kill.row != NoRow) {
			if ((claim.flags & ClaimDiablo) != 0)
				kill.banked = table.values.diabloKill;
			else if ((claim.flags & ClaimUnique) != 0)
				kill.banked = table.values.uniqueKill;
		}
		if ((claim.flags & ClaimDiablo) == 0)
			return { kill };
		AwardMsg final = Answer(claim, AwardKind::Final);
		final.row = kill.row;
		final.value = static_cast<uint32_t>(EndReason::Diablo);
		return { kill, final };
	}
	case ClaimKind::PlayerKill: {
		const uint8_t killerRow = RowForSlot(table, claim.subject);
		if (killerRow == NoRow || killerRow == senderRow)
			return { Answer(claim, AwardKind::Reject) };
		AwardMsg award = Answer(claim, AwardKind::PlayerKill);
		award.row = killerRow;
		award.row2 = senderRow;
		const int32_t victimBanked = std::max<int32_t>(table.rows[senderRow].banked, 0);
		award.banked = static_cast<int16_t>(std::min<int32_t>(table.values.playerKillTransfer, victimBanked));
		bool counts = true;
		auto it = std::find_if(pvp.entries.begin(), pvp.entries.end(), [&](const PvpMemory::Entry &e) { return e.killer == killerRow && e.victim == senderRow; });
		if (it != pvp.entries.end() && nowMs - it->timeMs < static_cast<uint32_t>(table.values.pvpRepeatSeconds) * 1000U)
			counts = false;
		if (counts) {
			if (it != pvp.entries.end())
				it->timeMs = nowMs;
			else
				pvp.entries.push_back({ killerRow, senderRow, nowMs });
		}
		award.flags = counts ? 1 : 0;
		return { award };
	}
	case ClaimKind::Gold: {
		if (claim.value == 0)
			return { Answer(claim, AwardKind::Reject) };
		AwardMsg award = Answer(claim, AwardKind::Gold);
		award.row = senderRow;
		award.value = claim.value;
		return { award };
	}
	case ClaimKind::Potion: {
		AwardMsg award = Answer(claim, AwardKind::Potion);
		award.row = senderRow;
		return { award };
	}
	case ClaimKind::Identified: {
		if (SortedContains(table.identifiedSeeds, claim.value) || table.identifiedSeeds.size() >= MaxIdentifiedSeeds)
			return { Answer(claim, AwardKind::Reject) };
		AwardMsg award = Answer(claim, AwardKind::Identified);
		award.row = senderRow;
		award.value = claim.value;
		return { award };
	}
	case ClaimKind::SecondHand:
	case ClaimKind::RequestTable:
		break;
	}
	return { Answer(claim, AwardKind::Reject) };
}

bool ApplyAward(ScoreTable &table, const AwardMsg &award)
{
	switch (award.kind) {
	case AwardKind::AddRow: {
		if (award.row != table.rows.size() || table.rows.size() >= MaxRows)
			return false;
		for (ScoreRow &other : table.rows) {
			if (other.slot == award.slot)
				other.slot = NoSlot;
		}
		ScoreRow row;
		row.name = award.name.substr(0, MaxNameLength);
		row.slot = award.slot;
		row.family.assign(table.familyCount, 0);
		table.rows.push_back(std::move(row));
		return true;
	}
	case AwardKind::Seat:
		if (!ValidRow(table, award.row))
			return false;
		for (ScoreRow &other : table.rows) {
			if (other.slot == award.slot)
				other.slot = NoSlot;
		}
		table.rows[award.row].slot = award.slot;
		return true;
	case AwardKind::Kill:
		SortedInsert(table.doneMonsters, award.value, MaxDoneMonsters);
		if (award.row == NoRow)
			return true;
		if (!ValidRow(table, award.row))
			return false;
		{
			ScoreRow &row = table.rows[award.row];
			if ((award.flags & ClaimCounted) != 0) {
				SaturatingAdd(row.kills, 1);
				if (award.family < row.family.size())
					SaturatingAdd(row.family[award.family], 1);
			}
			row.banked += award.banked;
		}
		return true;
	case AwardKind::PlayerKill:
		if (!ValidRow(table, award.row) || !ValidRow(table, award.row2))
			return false;
		table.rows[award.row].banked += award.banked;
		table.rows[award.row2].banked -= award.banked;
		if (award.flags != 0)
			SaturatingAdd(table.rows[award.row].pvpKills, 1);
		return true;
	case AwardKind::Gold:
		if (!ValidRow(table, award.row))
			return false;
		SaturatingAdd(table.rows[award.row].gold, award.value);
		return true;
	case AwardKind::Potion:
		if (!ValidRow(table, award.row))
			return false;
		SaturatingAdd(table.rows[award.row].potions, 1);
		return true;
	case AwardKind::Identified:
		if (!ValidRow(table, award.row))
			return false;
		SaturatingAdd(table.rows[award.row].identified, 1);
		SortedInsert(table.identifiedSeeds, award.value, MaxIdentifiedSeeds);
		return true;
	case AwardKind::Reject:
		return true;
	case AwardKind::Final:
		if (award.value == static_cast<uint32_t>(EndReason::None) || award.value > static_cast<uint32_t>(EndReason::Host))
			return false;
		table.ended = static_cast<EndReason>(award.value);
		table.endRow = ValidRow(table, award.row) ? award.row : NoRow;
		return true;
	}
	return false;
}

std::vector<RankEntry> RankAward(const ScoreTable &table, Award award)
{
	std::vector<std::pair<uint32_t, uint8_t>> ranked;
	for (size_t i = 0; i < table.rows.size(); i++) {
		const uint32_t count = AwardCount(table.rows[i], award);
		if (count > 0)
			ranked.emplace_back(count, static_cast<uint8_t>(i));
	}
	std::stable_sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) { return a.first > b.first; });

	std::vector<RankEntry> entries;
	size_t place = 0;
	size_t i = 0;
	while (i < ranked.size() && place < NumRanks) {
		RankEntry entry;
		entry.place = static_cast<uint8_t>(place);
		entry.count = ranked[i].first;
		while (i < ranked.size() && ranked[i].first == entry.count) {
			entry.rows.push_back(ranked[i].second);
			i++;
		}
		entry.points = entry.rows.size() == 1 ? RankPoints(table.values, award, place) : 0;
		place += entry.rows.size();
		entries.push_back(std::move(entry));
	}
	return entries;
}

std::vector<FamilyLead> FamilyLeads(const ScoreTable &table, const std::vector<uint8_t> &familyOrder)
{
	std::vector<FamilyLead> leads;
	for (const uint8_t family : familyOrder) {
		if (family >= table.familyCount)
			continue;
		FamilyLead lead;
		lead.family = family;
		size_t leaders = 0;
		for (size_t i = 0; i < table.rows.size(); i++) {
			const uint16_t count = family < table.rows[i].family.size() ? table.rows[i].family[family] : 0;
			if (count == 0 || count < lead.count)
				continue;
			if (count > lead.count) {
				lead.count = count;
				lead.row = static_cast<uint8_t>(i);
				leaders = 1;
			} else {
				leaders++;
			}
		}
		if (lead.count == 0)
			continue;
		if (leaders > 1)
			lead.row = NoRow;
		else
			lead.points = table.values.familyLead;
		leads.push_back(lead);
	}
	return leads;
}

namespace {

std::vector<uint8_t> AllFamilies(const ScoreTable &table)
{
	std::vector<uint8_t> order(table.familyCount);
	for (size_t i = 0; i < order.size(); i++)
		order[i] = static_cast<uint8_t>(i);
	return order;
}

void AddFamilyPoints(std::vector<int32_t> &out, const std::vector<FamilyLead> &leads, size_t first, size_t count)
{
	for (size_t i = first; i < first + count && i < leads.size(); i++) {
		if (!leads[i].tied() && leads[i].row < out.size())
			out[leads[i].row] += leads[i].points;
	}
}

void AddAwardPoints(std::vector<int32_t> &out, const ScoreTable &table, Award award)
{
	for (const RankEntry &entry : RankAward(table, award)) {
		if (!entry.tied() && entry.rows[0] < out.size())
			out[entry.rows[0]] += entry.points;
	}
}

} // namespace

std::vector<RowTotal> Totals(const ScoreTable &table)
{
	const size_t n = table.rows.size();
	std::vector<int32_t> families(n, 0);
	std::vector<int32_t> awards(n, 0);
	const std::vector<FamilyLead> leads = FamilyLeads(table, AllFamilies(table));
	AddFamilyPoints(families, leads, 0, leads.size());
	for (size_t a = 0; a < NumAwards; a++)
		AddAwardPoints(awards, table, static_cast<Award>(a));

	std::vector<RowTotal> totals(n);
	for (size_t i = 0; i < n; i++) {
		totals[i].banked = table.rows[i].banked;
		totals[i].families = families[i];
		totals[i].awards = awards[i];
		totals[i].total = totals[i].banked + families[i] + awards[i];
	}
	for (size_t i = 0; i < n; i++) {
		size_t better = 0;
		for (size_t j = 0; j < n; j++) {
			if (totals[j].total > totals[i].total)
				better++;
		}
		totals[i].place = static_cast<uint8_t>(std::min<size_t>(better, 255));
	}
	return totals;
}

std::vector<uint8_t> OverallOrder(const std::vector<RowTotal> &totals)
{
	std::vector<uint8_t> order(totals.size());
	for (size_t i = 0; i < order.size(); i++)
		order[i] = static_cast<uint8_t>(i);
	std::stable_sort(order.begin(), order.end(), [&](uint8_t a, uint8_t b) { return totals[a].total > totals[b].total; });
	return order;
}

std::vector<LiveRow> LiveBoard(const ScoreTable &table)
{
	const std::vector<RowTotal> totals = Totals(table);
	std::vector<LiveRow> rows;
	for (const uint8_t r : OverallOrder(totals)) {
		LiveRow live;
		live.row = r;
		live.overallPlace = totals[r].place;
		live.banked = table.rows[r].banked;
		live.left = table.rows[r].slot == NoSlot;
		rows.push_back(live);
	}
	for (size_t a = 0; a < NumAwards; a++) {
		for (const RankEntry &entry : RankAward(table, static_cast<Award>(a))) {
			for (const uint8_t r : entry.rows) {
				for (LiveRow &live : rows) {
					if (live.row == r)
						live.awards[a] = { entry.place, entry.tied() };
				}
			}
		}
	}
	return rows;
}

std::vector<LiveFamily> LiveFamilies(const ScoreTable &table, const std::vector<uint8_t> &familyOrder)
{
	std::vector<LiveFamily> out;
	for (const FamilyLead &lead : FamilyLeads(table, familyOrder)) {
		LiveFamily family;
		family.family = lead.family;
		for (size_t r = 0; r < table.rows.size(); r++) {
			if (lead.family < table.rows[r].family.size() && table.rows[r].family[lead.family] == lead.count)
				family.rows.push_back(static_cast<uint8_t>(r));
		}
		out.push_back(std::move(family));
	}
	return out;
}

std::vector<RevealStep> BuildRevealSteps(const ScoreTable &table, const std::vector<uint8_t> &familyOrder)
{
	std::vector<RevealStep> steps;
	steps.push_back({ RevealStepKind::Banked });
	if (table.values.familyLead != 0) {
		const size_t leads = FamilyLeads(table, familyOrder).size();
		for (size_t first = 0; first < leads; first += FamiliesPerRevealStep) {
			RevealStep step;
			step.kind = RevealStepKind::Families;
			step.first = first;
			step.count = std::min(FamiliesPerRevealStep, leads - first);
			steps.push_back(step);
		}
	}
	for (size_t a = 0; a < NumAwards; a++) {
		const auto award = static_cast<Award>(a);
		if (!AwardHasPoints(table.values, award))
			continue;
		RevealStep step;
		step.kind = RevealStepKind::Award;
		step.award = award;
		steps.push_back(step);
	}
	steps.push_back({ RevealStepKind::Final });
	return steps;
}

std::vector<int32_t> RunningTotals(const ScoreTable &table, const std::vector<uint8_t> &familyOrder, const std::vector<RevealStep> &steps, size_t shown)
{
	std::vector<int32_t> out(table.rows.size(), 0);
	const std::vector<FamilyLead> leads = FamilyLeads(table, familyOrder);
	for (size_t s = 0; s < shown && s < steps.size(); s++) {
		const RevealStep &step = steps[s];
		switch (step.kind) {
		case RevealStepKind::Banked:
			for (size_t i = 0; i < out.size(); i++)
				out[i] += table.rows[i].banked;
			break;
		case RevealStepKind::Families:
			AddFamilyPoints(out, leads, step.first, step.count);
			break;
		case RevealStepKind::Award:
			AddAwardPoints(out, table, step.award);
			break;
		case RevealStepKind::Final:
			break;
		}
	}
	return out;
}

std::vector<uint8_t> SerializeTable(const ScoreTable &table)
{
	Writer w;
	w.u8(TableFormatVersion);
	w.u8(table.epoch);
	w.u16(table.seq);
	w.u8(static_cast<uint8_t>(table.ended));
	w.u8(table.endRow);
	uint8_t values[SerializedValuesSize];
	SerializeValues(table.values, values);
	for (const uint8_t v : values)
		w.u8(v);
	w.u8(table.familyCount);
	w.u8(static_cast<uint8_t>(table.rows.size()));
	for (const ScoreRow &row : table.rows) {
		const size_t len = std::min(row.name.size(), MaxNameLength);
		w.u8(static_cast<uint8_t>(len));
		for (size_t i = 0; i < len; i++)
			w.u8(static_cast<uint8_t>(row.name[i]));
		w.u8(row.slot);
		w.u32(static_cast<uint32_t>(row.banked));
		w.u32(row.gold);
		w.u16(row.pvpKills);
		w.u16(row.kills);
		w.u16(row.potions);
		w.u16(row.identified);
		for (size_t f = 0; f < table.familyCount; f++)
			w.u16(f < row.family.size() ? row.family[f] : 0);
	}
	w.set(table.doneMonsters);
	w.set(table.secondHand);
	w.set(table.identifiedSeeds);
	return std::move(w.bytes);
}

std::optional<ScoreTable> DeserializeTable(const uint8_t *data, size_t size)
{
	Reader r(data, size);
	if (r.u8() != TableFormatVersion)
		return std::nullopt;
	ScoreTable table;
	table.epoch = r.u8();
	table.seq = r.u16();
	const uint8_t ended = r.u8();
	if (ended > static_cast<uint8_t>(EndReason::Host))
		return std::nullopt;
	table.ended = static_cast<EndReason>(ended);
	table.endRow = r.u8();
	const uint8_t *values = r.take(SerializedValuesSize);
	if (values == nullptr)
		return std::nullopt;
	table.values = DeserializeValues(values);
	table.familyCount = r.u8();
	const uint8_t rowCount = r.u8();
	if (!r.ok || rowCount > MaxRows)
		return std::nullopt;
	table.rows.resize(rowCount);
	for (ScoreRow &row : table.rows) {
		const uint8_t len = r.u8();
		if (len > MaxNameLength)
			return std::nullopt;
		const uint8_t *name = r.take(len);
		if (name == nullptr)
			return std::nullopt;
		row.name.assign(reinterpret_cast<const char *>(name), len);
		row.slot = r.u8();
		row.banked = static_cast<int32_t>(r.u32());
		row.gold = r.u32();
		row.pvpKills = r.u16();
		row.kills = r.u16();
		row.potions = r.u16();
		row.identified = r.u16();
		row.family.resize(table.familyCount);
		for (uint16_t &count : row.family)
			count = r.u16();
	}
	if (!r.ok || (table.endRow != NoRow && table.endRow >= table.rows.size()))
		return std::nullopt;
	if (!r.set(table.doneMonsters, MaxDoneMonsters) || !r.set(table.secondHand, MaxSecondHand) || !r.set(table.identifiedSeeds, MaxIdentifiedSeeds))
		return std::nullopt;
	return table;
}

std::string DescribeTable(const ScoreTable &table)
{
	std::string out = "values=";
	for (size_t i = 0; i < NumValueSettings; i++) {
		if (i != 0)
			out += ',';
		out += std::to_string(GetValue(table.values, i));
	}
	out += " end=" + std::to_string(static_cast<int>(table.ended)) + "/" + std::to_string(table.endRow);
	out += " done=" + std::to_string(table.doneMonsters.size());
	out += " second=" + std::to_string(table.secondHand.size());
	out += " ided=" + std::to_string(table.identifiedSeeds.size());
	const std::vector<RowTotal> totals = Totals(table);
	for (size_t i = 0; i < table.rows.size(); i++) {
		const ScoreRow &row = table.rows[i];
		out += " r" + std::to_string(i) + ":" + row.name + ":" + std::to_string(row.slot);
		out += ":b" + std::to_string(row.banked) + ":g" + std::to_string(row.gold) + ":p" + std::to_string(row.pvpKills);
		out += ":k" + std::to_string(row.kills) + ":d" + std::to_string(row.potions) + ":i" + std::to_string(row.identified) + ":f";
		for (size_t f = 0; f < row.family.size(); f++) {
			if (row.family[f] != 0)
				out += std::to_string(f) + "x" + std::to_string(row.family[f]) + ".";
		}
		out += ":t" + std::to_string(totals[i].total) + "@" + std::to_string(totals[i].place + 1);
	}
	return out;
}

} // namespace devilution::score
