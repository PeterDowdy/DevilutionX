/**
 * @file score/score_board.cpp
 *
 * Multiplayer scoring: the leader board and the stepped end reveal.
 *
 * Laid out for HD screens (Peter 2026-11-20: "assume a hi-def monitor for all players"): one dark box over the play area
 * above the main panel, a column per player.
 * During play (Peter 2026-11-20): each player's place overall, in each award and in each family lead, and each player's
 * banked points (uniques, Diablo, player-kill transfers; their events are announced in chat anyway). No other count is
 * drawn, the player's own included.
 * At the end: the stepped reveal (banked standings, family leads six a step, the five awards, the final table), about two
 * seconds a step, any arrow key jumps to the last step; afterwards the board key reopens the final table with a Families
 * page and an Awards page that show every player's counts.
 */
#include <algorithm>
#include <string>
#include <vector>

#ifdef USE_SDL3
#include <SDL3/SDL_timer.h>
#else
#include <SDL.h>
#endif

#include "DiabloUI/ui_flags.hpp"
#include "control/control.hpp"
#include "engine/palette.h"
#include "engine/render/primitive_render.hpp"
#include "engine/render/text_render.hpp"
#include "help.h"
#include "options.h"
#include "qol/chatlog.h"
#include "score/score.h"
#include "score/score_internal.hpp"
#include "score/score_rules.hpp"
#include "utils/format.hpp"
#include "utils/language.h"
#include "utils/str_cat.hpp"
#include "utils/ui_fwd.h"

namespace devilution {

bool ScoreBoardFlag = false;

namespace {

using score::Award;
using score::EndReason;
using score::NoRow;
using score::RevealStep;
using score::RevealStepKind;
using score::ScoreTable;

constexpr int Margin = 16;
constexpr int MaxBoardWidth = 1200;
constexpr int Padding = 16;
constexpr int LabelWidth = 240;
constexpr int MaxColumnWidth = 190;
constexpr uint32_t RevealStepMs = 2000;
constexpr uint8_t BorderColor = PAL8_YELLOW + 2;
constexpr uint8_t RuleColor = PAL8_YELLOW + 4;

struct Cell {
	std::string text;
	UiFlags color = UiFlags::ColorWhite;
};

/**
 * One line of the board: a label and either a text, or one cell per player column (a table row).
 * `heading` lines are drawn in blue across the whole width.
 */
struct BoardLine {
	std::string label;
	std::string text;
	UiFlags color = UiFlags::ColorWhite;
	std::vector<Cell> cells;
	bool heading = false;
};

enum class FinalPage : uint8_t {
	Table,
	Families,
	Awards,
};

FinalPage EndPage = FinalPage::Table;
size_t SkipLines = 0;
bool Revealing = false;
size_t RevealIndex = 0;
uint32_t RevealStepStart = 0;

int LineHeight()
{
	return IsSmallFontTall() ? 18 : 15;
}

Rectangle BoardRect()
{
	const int screenWidth = GetScreenWidth();
	const int width = std::min(screenWidth - (2 * Margin), MaxBoardWidth);
	const int bottom = GetMainPanel().position.y - Margin;
	return { { (screenWidth - width) / 2, Margin }, { width, std::max(bottom - Margin, 200) } };
}

std::string_view AwardName(Award award)
{
	switch (award) {
	case Award::Identified:
		return _("Items identified");
	case Award::Potions:
		return _("Potions drunk");
	case Award::Gold:
		return _("Gold found");
	case Award::PvpKills:
		return _("Player kills");
	case Award::Kills:
		return _("Monster kills");
	}
	return {};
}

std::string Ordinal(size_t place)
{
	switch (place) {
	case 0:
		return std::string(_("1st"));
	case 1:
		return std::string(_("2nd"));
	case 2:
		return std::string(_("3rd"));
	default:
		return FormatRuntime(_("{:d}th"), place + 1);
	}
}

std::string RowName(const ScoreTable &table, uint8_t row)
{
	if (row >= table.rows.size())
		return std::string(_("Nobody"));
	return table.rows[row].name;
}

std::string RowLabel(const ScoreTable &table, uint8_t row)
{
	if (row < table.rows.size() && table.rows[row].slot == score::NoSlot)
		return StrCat(table.rows[row].name, " ", _("(left)"));
	return RowName(table, row);
}

std::string JoinRows(const ScoreTable &table, const std::vector<uint8_t> &rows, std::string_view separator)
{
	std::string out;
	for (const uint8_t row : rows) {
		if (!out.empty())
			out += separator;
		out += RowLabel(table, row);
	}
	return out;
}

/** The header row of a table: one column per row of `order`. */
BoardLine NamesRow(const ScoreTable &table, const std::vector<uint8_t> &order)
{
	BoardLine line;
	for (const uint8_t row : order)
		line.cells.push_back({ RowLabel(table, row), UiFlags::ColorWhitegold });
	return line;
}

/** During play: places, banked points, award places, family leads; columns in overall order. */
std::vector<BoardLine> LiveLines(const ScoreTable &table)
{
	const std::vector<score::LiveRow> live = score::LiveBoard(table);
	std::vector<uint8_t> order;
	for (const score::LiveRow &row : live)
		order.push_back(row.row);
	auto column = [&](uint8_t row) { return static_cast<size_t>(std::find(order.begin(), order.end(), row) - order.begin()); };

	std::vector<BoardLine> lines;
	lines.push_back(NamesRow(table, order));
	BoardLine place { std::string(_("Place")), "", UiFlags::ColorWhite };
	BoardLine banked { std::string(_("Banked points")), "", UiFlags::ColorWhite };
	for (const score::LiveRow &row : live) {
		place.cells.push_back({ Ordinal(row.overallPlace), row.overallPlace == 0 ? UiFlags::ColorGold : UiFlags::ColorWhite });
		banked.cells.push_back({ StrCat(row.banked) });
	}
	lines.push_back(std::move(place));
	lines.push_back(std::move(banked));
	lines.push_back({});

	lines.push_back({ "", std::string(_("Awards (places only)")), UiFlags::ColorBlue, {}, true });
	for (size_t a = 0; a < score::NumAwards; a++) {
		BoardLine line { std::string(AwardName(static_cast<Award>(a))), "", UiFlags::ColorWhite };
		for (const score::LiveRow &row : live) {
			const score::LivePlace &p = row.awards[a];
			if (!p.placed())
				line.cells.push_back({ "-", UiFlags::ColorWhite });
			else if (p.tied)
				line.cells.push_back({ FormatRuntime(_("tied {:s}"), Ordinal(p.place)), UiFlags::ColorWhite });
			else
				line.cells.push_back({ Ordinal(p.place), p.place == 0 ? UiFlags::ColorGold : UiFlags::ColorWhite });
		}
		lines.push_back(std::move(line));
	}
	lines.push_back({});

	lines.push_back({ "", std::string(_("Most kills in a monster family")), UiFlags::ColorBlue, {}, true });
	const std::vector<score::LiveFamily> families = score::LiveFamilies(table, ScoreFamilyOrder());
	if (families.empty())
		lines.push_back({ "", std::string(_("No family has been hunted yet.")) });
	for (const score::LiveFamily &family : families) {
		BoardLine line { std::string(ScoreFamilyName(family.family)), "", UiFlags::ColorWhite };
		line.cells.assign(order.size(), Cell {});
		for (const uint8_t row : family.rows) {
			const size_t c = column(row);
			if (c < line.cells.size())
				line.cells[c] = family.tied() ? Cell { std::string(_("tied")), UiFlags::ColorWhite } : Cell { std::string(_("leads")), UiFlags::ColorGold };
		}
		lines.push_back(std::move(line));
	}
	return lines;
}

std::string EndReasonText(const ScoreTable &table)
{
	if (table.ended == EndReason::Host)
		return std::string(_("Ended by the host"));
	if (table.endRow != NoRow)
		return FormatRuntime(_("{:s} killed Diablo"), RowLabel(table, table.endRow));
	return std::string(_("Diablo is dead"));
}

void AppendBanked(std::vector<BoardLine> &lines, const ScoreTable &table)
{
	lines.push_back({ "", EndReasonText(table), UiFlags::ColorWhitegold });
	lines.push_back({ "", std::string(_("Banked points (uniques, Diablo, player kills):")), UiFlags::ColorBlue, {}, true });
	std::vector<uint8_t> order(table.rows.size());
	for (size_t i = 0; i < order.size(); i++)
		order[i] = static_cast<uint8_t>(i);
	std::stable_sort(order.begin(), order.end(), [&](uint8_t a, uint8_t b) { return table.rows[a].banked > table.rows[b].banked; });
	for (const uint8_t row : order)
		lines.push_back({ RowLabel(table, row), StrCat(table.rows[row].banked) });
}

void AppendFamilies(std::vector<BoardLine> &lines, const ScoreTable &table, size_t first, size_t count)
{
	const std::vector<score::FamilyLead> leads = score::FamilyLeads(table, ScoreFamilyOrder());
	for (size_t i = first; i < first + count && i < leads.size(); i++) {
		const score::FamilyLead &lead = leads[i];
		std::string text = lead.tied()
		    ? FormatRuntime(ngettext("tied at {:d} kill, no point", "tied at {:d} kills, no point", lead.count), lead.count)
		    : FormatRuntime(ngettext("{:s}, {:d} kill, +{:d}", "{:s}, {:d} kills, +{:d}", lead.count), RowLabel(table, lead.row), lead.count, lead.points);
		lines.push_back({ std::string(ScoreFamilyName(lead.family)), std::move(text) });
	}
}

void AppendAward(std::vector<BoardLine> &lines, const ScoreTable &table, Award award)
{
	lines.push_back({ "", std::string(AwardName(award)), UiFlags::ColorBlue, {}, true });
	const std::vector<score::RankEntry> ranks = score::RankAward(table, award);
	if (ranks.empty())
		lines.push_back({ "", std::string(_("Nobody")) });
	for (const score::RankEntry &entry : ranks) {
		std::string text = entry.tied()
		    ? FormatRuntime(_("tied: {:s} at {:d}, no points"), JoinRows(table, entry.rows, ", "), entry.count)
		    : FormatRuntime(_("{:s}, {:d}, +{:d}"), RowLabel(table, entry.rows[0]), entry.count, entry.points);
		lines.push_back({ Ordinal(entry.place), std::move(text) });
	}
}

/** The final table: a column per player (final order), banked + families + awards = total. */
void AppendFinalTable(std::vector<BoardLine> &lines, const ScoreTable &table)
{
	const std::vector<score::RowTotal> totals = score::Totals(table);
	const std::vector<uint8_t> order = score::OverallOrder(totals);
	std::vector<uint8_t> winners;
	for (const uint8_t row : order) {
		if (totals[row].place == 0)
			winners.push_back(row);
	}
	if (winners.size() == 1)
		lines.push_back({ "", FormatRuntime(_("{:s} wins!"), RowLabel(table, winners[0])), UiFlags::ColorWhitegold });
	else if (!winners.empty())
		lines.push_back({ "", FormatRuntime(_("Tied for first: {:s}"), JoinRows(table, winners, ", ")), UiFlags::ColorWhitegold });
	lines.push_back({});
	lines.push_back(NamesRow(table, order));
	BoardLine place { std::string(_("Place")) };
	BoardLine banked { std::string(_("Banked points")) };
	BoardLine families { std::string(_("Family leads")) };
	BoardLine awards { std::string(_("Awards")) };
	BoardLine total { std::string(_("Total")) };
	for (const uint8_t row : order) {
		const score::RowTotal &t = totals[row];
		place.cells.push_back({ Ordinal(t.place), t.place == 0 ? UiFlags::ColorGold : UiFlags::ColorWhite });
		banked.cells.push_back({ StrCat(t.banked) });
		families.cells.push_back({ StrCat("+", t.families) });
		awards.cells.push_back({ StrCat("+", t.awards) });
		total.cells.push_back({ StrCat(t.total), UiFlags::ColorGold });
	}
	lines.push_back(std::move(place));
	lines.push_back(std::move(banked));
	lines.push_back(std::move(families));
	lines.push_back(std::move(awards));
	lines.push_back(std::move(total));
}

/** Every player's kills per family; the leader's count carries the point (final Families page). */
void AppendFamilyCounts(std::vector<BoardLine> &lines, const ScoreTable &table)
{
	const std::vector<uint8_t> order = score::OverallOrder(score::Totals(table));
	lines.push_back(NamesRow(table, order));
	const std::vector<score::FamilyLead> leads = score::FamilyLeads(table, ScoreFamilyOrder());
	if (leads.empty())
		lines.push_back({ "", std::string(_("No family was hunted.")) });
	for (const score::FamilyLead &lead : leads) {
		BoardLine line { std::string(ScoreFamilyName(lead.family)) };
		for (const uint8_t row : order) {
			const uint16_t count = lead.family < table.rows[row].family.size() ? table.rows[row].family[lead.family] : 0;
			if (!lead.tied() && lead.row == row)
				line.cells.push_back({ StrCat(count, "  +", lead.points), UiFlags::ColorGold });
			else
				line.cells.push_back({ count == 0 ? std::string("-") : StrCat(count) });
		}
		lines.push_back(std::move(line));
	}
}

/** Every player's count per award with the points it earned (final Awards page). */
void AppendAwardCounts(std::vector<BoardLine> &lines, const ScoreTable &table)
{
	const std::vector<uint8_t> order = score::OverallOrder(score::Totals(table));
	lines.push_back(NamesRow(table, order));
	for (size_t a = 0; a < score::NumAwards; a++) {
		const auto award = static_cast<Award>(a);
		const std::vector<score::RankEntry> ranks = score::RankAward(table, award);
		BoardLine line { std::string(AwardName(award)) };
		for (const uint8_t row : order) {
			int points = 0;
			for (const score::RankEntry &entry : ranks) {
				if (!entry.tied() && entry.rows[0] == row)
					points = entry.points;
			}
			const uint32_t count = score::AwardCount(table.rows[row], award);
			if (points > 0)
				line.cells.push_back({ StrCat(count, "  +", points), UiFlags::ColorGold });
			else
				line.cells.push_back({ StrCat(count) });
		}
		lines.push_back(std::move(line));
	}
}

std::vector<BoardLine> RevealLines(const ScoreTable &table, const RevealStep &step)
{
	std::vector<BoardLine> lines;
	switch (step.kind) {
	case RevealStepKind::Banked:
		AppendBanked(lines, table);
		break;
	case RevealStepKind::Families:
		lines.push_back({ "", std::string(_("Most kills in a monster family:")), UiFlags::ColorBlue, {}, true });
		AppendFamilies(lines, table, step.first, step.count);
		break;
	case RevealStepKind::Award:
		AppendAward(lines, table, step.award);
		break;
	case RevealStepKind::Final:
		AppendFinalTable(lines, table);
		break;
	}
	return lines;
}

std::vector<BoardLine> EndLines(const ScoreTable &table)
{
	std::vector<BoardLine> lines;
	switch (EndPage) {
	case FinalPage::Table:
		AppendFinalTable(lines, table);
		break;
	case FinalPage::Families:
		AppendFamilyCounts(lines, table);
		break;
	case FinalPage::Awards:
		AppendAwardCounts(lines, table);
		break;
	}
	return lines;
}

/** "Anna 12   Bo 9" during the reveal: each total counting up as the steps reveal points. */
std::string RunningTotalsText(const ScoreTable &table, const std::vector<RevealStep> &steps)
{
	const std::vector<int32_t> running = score::RunningTotals(table, ScoreFamilyOrder(), steps, RevealIndex + 1);
	std::string out;
	for (size_t row = 0; row < table.rows.size(); row++)
		StrAppend(out, out.empty() ? "" : "      ", table.rows[row].name, " ", running[row]);
	return out;
}

void AdvanceReveal(size_t stepCount)
{
	if (!Revealing)
		return;
	const uint32_t now = static_cast<uint32_t>(SDL_GetTicks());
	while (RevealIndex + 1 < stepCount && now - RevealStepStart >= RevealStepMs) {
		RevealIndex++;
		RevealStepStart += RevealStepMs;
	}
	if (RevealIndex + 1 >= stepCount)
		Revealing = false;
}

void SkipReveal()
{
	Revealing = false;
	RevealIndex = 0;
	EndPage = FinalPage::Table;
	SkipLines = 0;
}

void DrawBox(const Surface &out, const Rectangle &box)
{
	// Twice half transparent: dark enough to read over the dungeon, the game still shows through a little.
	DrawHalfTransparentRectTo(out, box.position.x, box.position.y, box.size.width, box.size.height);
	DrawHalfTransparentRectTo(out, box.position.x, box.position.y, box.size.width, box.size.height);
	UnsafeDrawBorder2px(out, box, BorderColor);
}

void DrawLines(const Surface &out, const std::vector<BoardLine> &lines, Rectangle area)
{
	const int lineHeight = LineHeight();
	const int visible = std::max(1, area.size.height / lineHeight);
	const size_t maxSkip = lines.size() > static_cast<size_t>(visible) ? lines.size() - visible : 0;
	SkipLines = std::min(SkipLines, maxSkip);
	for (int i = 0; i < visible; i++) {
		const size_t index = i + SkipLines;
		if (index >= lines.size())
			break;
		const BoardLine &line = lines[index];
		const int y = area.position.y + (i * lineHeight);
		const int x = area.position.x;
		if (!line.cells.empty()) {
			if (!line.label.empty())
				DrawString(out, line.label, { { x, y }, { LabelWidth, lineHeight } }, { .flags = UiFlags::ColorWhitegold });
			const int columns = static_cast<int>(line.cells.size());
			const int columnWidth = std::min(MaxColumnWidth, (area.size.width - LabelWidth) / std::max(columns, 1));
			for (int c = 0; c < columns; c++) {
				DrawString(out, line.cells[c].text, { { x + LabelWidth + (c * columnWidth), y }, { columnWidth - 8, lineHeight } }, { .flags = line.cells[c].color });
			}
		} else if (line.label.empty()) {
			DrawString(out, line.text, { { x, y }, { area.size.width, lineHeight } }, { .flags = line.color });
		} else {
			DrawString(out, line.label, { { x, y }, { LabelWidth, lineHeight } }, { .flags = UiFlags::ColorWhitegold });
			DrawString(out, line.text, { { x + LabelWidth, y }, { area.size.width - LabelWidth, lineHeight } }, { .flags = line.color });
		}
	}
}

} // namespace

void ScoreBoardStartReveal()
{
	if (!ScoringActive())
		return;
	Revealing = true;
	RevealIndex = 0;
	RevealStepStart = static_cast<uint32_t>(SDL_GetTicks());
	EndPage = FinalPage::Table;
	SkipLines = 0;
	ChatLogFlag = false;
	HelpFlag = false;
	ScoreBoardFlag = true;
}

void ScoreBoardReset()
{
	ScoreBoardFlag = false;
	Revealing = false;
	RevealIndex = 0;
	EndPage = FinalPage::Table;
	SkipLines = 0;
}

void ToggleScoreBoard()
{
	if (!ScoringActive())
		return;
	if (ScoreBoardFlag && Revealing) {
		SkipReveal();
		return;
	}
	ScoreBoardFlag = !ScoreBoardFlag;
	if (ScoreBoardFlag) {
		ChatLogFlag = false;
		HelpFlag = false;
		SkipLines = 0;
	}
}

void ScoreBoardKey(int direction)
{
	if (!ScoreBoardFlag)
		return;
	if (Revealing) {
		SkipReveal();
		return;
	}
	const ScoreTable *table = ScoreTableIfAny();
	const bool ended = table != nullptr && table->ended != EndReason::None;
	switch (direction) {
	case 0:
		if (SkipLines > 0)
			SkipLines--;
		break;
	case 1:
		SkipLines++;
		break;
	case 2:
	case 3:
		if (ended) {
			const int step = direction == 2 ? -1 : 1;
			EndPage = static_cast<FinalPage>((static_cast<int>(EndPage) + 3 + step) % 3);
			SkipLines = 0;
		}
		break;
	default:
		break;
	}
}

void DrawScoreBoard(const Surface &out)
{
	const ScoreTable *table = ScoreTableIfAny();
	const Rectangle box = BoardRect();
	DrawBox(out, box);

	const int lineHeight = LineHeight();
	const int x = box.position.x + Padding;
	const int width = box.size.width - (2 * Padding);

	std::string title;
	std::vector<BoardLine> lines;
	std::string footer;
	std::string totalsLine;
	if (table == nullptr) {
		title = _("Score Board");
		lines.push_back({ "", std::string(_("The score table has not arrived yet.")) });
	} else if (table->ended == EndReason::None) {
		title = _("Score Board");
		lines = LiveLines(*table);
		footer = _("Places and banked points only; the counts are revealed when the game ends. Up/down: scroll.");
	} else {
		const std::vector<RevealStep> steps = score::BuildRevealSteps(*table, ScoreFamilyOrder());
		AdvanceReveal(steps.size());
		if (Revealing) {
			title = StrCat(_("Final Scores"), " (", RevealIndex + 1, "/", steps.size(), ")");
			lines = RevealLines(*table, steps[RevealIndex]);
			totalsLine = RunningTotalsText(*table, steps);
			footer = _("Press an arrow key to see the final table.");
		} else {
			switch (EndPage) {
			case FinalPage::Table:
				title = _("Final Scores");
				break;
			case FinalPage::Families:
				title = _("Final Scores: families");
				break;
			case FinalPage::Awards:
				title = _("Final Scores: awards");
				break;
			}
			lines = EndLines(*table);
			footer = _("Left/right: table, families, awards. Up/down: scroll.");
		}
	}

	int y = box.position.y + Padding;
	DrawString(out, title, { { x, y }, { width, lineHeight } }, { .flags = UiFlags::ColorWhitegold | UiFlags::AlignCenter });
	y += lineHeight + 6;
	DrawHorizontalLine(out, { x, y }, width, RuleColor);
	y += 8;
	if (!totalsLine.empty()) {
		// The totals counting up go at the top of the box: the chat lines of the end are drawn over the bottom of the screen.
		DrawString(out, totalsLine, { { x, y }, { width, lineHeight } }, { .flags = UiFlags::ColorGold | UiFlags::AlignCenter });
		y += lineHeight + (lineHeight / 2);
	}
	const int footerY = box.position.y + box.size.height - Padding - lineHeight;
	DrawLines(out, lines, { { x, y }, { width, footerY - 6 - y } });
	if (!footer.empty()) {
		DrawHorizontalLine(out, { x, footerY - 4 }, width, RuleColor);
		DrawString(out, footer, { { x, footerY }, { width, lineHeight } }, { .flags = UiFlags::ColorWhitegold | UiFlags::AlignCenter });
	}
}

} // namespace devilution
