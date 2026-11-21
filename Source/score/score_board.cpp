/**
 * @file score/score_board.cpp
 *
 * Multiplayer scoring: the leader board and the stepped end reveal, drawn in the chat-log text box.
 *
 * During play (Peter 2026-11-20): places only. Who is first / second / third in each award, who leads each family
 * (or "tied"), and the overall order. No counts are drawn, the player's own included.
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
#include "engine/render/text_render.hpp"
#include "help.h"
#include "minitext.h"
#include "options.h"
#include "qol/chatlog.h"
#include "score/score.h"
#include "score/score_internal.hpp"
#include "score/score_rules.hpp"
#include "stores.h"
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

constexpr int PaddingTop = 32;
constexpr int PaddingLeft = 32;
constexpr int PanelHeight = 297;
constexpr int ContentTextWidth = 577;
constexpr int LabelWidth = 150;
constexpr uint32_t RevealStepMs = 2000;

struct BoardLine {
	std::string label;
	std::string text;
	UiFlags color = UiFlags::ColorWhite;
};

enum class Page : uint8_t {
	Standings,
	Families,
};
enum class FinalPage : uint8_t {
	Table,
	Families,
	Awards,
};

Page LivePage = Page::Standings;
FinalPage EndPage = FinalPage::Table;
size_t SkipLines = 0;
bool Revealing = false;
size_t RevealIndex = 0;
uint32_t RevealStepStart = 0;

int LineHeight()
{
	return IsSmallFontTall() ? 18 : 14;
}

int HeaderHeight()
{
	return PaddingTop + LineHeight() + 24 + 6;
}

int NumVisibleLines()
{
	const int contents = PanelHeight - HeaderHeight() - 6 - 24 - 12;
	return ((contents - 1) / LineHeight()) + 1;
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

std::string_view PlaceName(size_t place)
{
	switch (place) {
	case 0:
		return _("1st");
	case 1:
		return _("2nd");
	default:
		return _("3rd");
	}
}

std::string RowLabel(const ScoreTable &table, uint8_t row)
{
	if (row >= table.rows.size())
		return std::string(_("Nobody"));
	const score::ScoreRow &r = table.rows[row];
	if (r.slot == score::NoSlot)
		return StrCat(r.name, " ", _("(left)"));
	return r.name;
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

/** "1. Anna   2. Bo = Cy" (places only, no counts). */
std::string PlacesText(const ScoreTable &table, const std::vector<score::RankEntry> &ranks)
{
	if (ranks.empty())
		return "-";
	std::string out;
	for (const score::RankEntry &entry : ranks) {
		if (!out.empty())
			out += "    ";
		StrAppend(out, entry.place + 1, ". ", JoinRows(table, entry.rows, " = "));
	}
	return out;
}

std::string OverallText(const ScoreTable &table)
{
	const std::vector<score::RowTotal> totals = score::Totals(table);
	std::string out;
	for (const uint8_t row : score::OverallOrder(totals)) {
		if (!out.empty())
			out += "    ";
		StrAppend(out, totals[row].place + 1, ". ", RowLabel(table, row));
	}
	return out.empty() ? "-" : out;
}

std::vector<BoardLine> LiveLines(const ScoreTable &table)
{
	std::vector<BoardLine> lines;
	if (LivePage == Page::Standings) {
		lines.push_back({ std::string(_("Overall")), OverallText(table), UiFlags::ColorWhitegold });
		lines.push_back({});
		for (size_t a = 0; a < score::NumAwards; a++) {
			const auto award = static_cast<Award>(a);
			lines.push_back({ std::string(AwardName(award)), PlacesText(table, score::RankAward(table, award)) });
		}
		return lines;
	}
	const std::vector<score::FamilyLead> leads = score::FamilyLeads(table, ScoreFamilyOrder());
	if (leads.empty())
		lines.push_back({ "", std::string(_("No family has been hunted yet.")) });
	for (const score::FamilyLead &lead : leads)
		lines.push_back({ std::string(ScoreFamilyName(lead.family)), lead.tied() ? std::string(_("tied")) : RowLabel(table, lead.row) });
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
	lines.push_back({ "", std::string(_("Banked points (uniques, Diablo, player kills):")), UiFlags::ColorBlue });
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
	lines.push_back({ "", std::string(AwardName(award)), UiFlags::ColorBlue });
	const std::vector<score::RankEntry> ranks = score::RankAward(table, award);
	if (ranks.empty())
		lines.push_back({ "", std::string(_("Nobody")) });
	for (const score::RankEntry &entry : ranks) {
		std::string text = entry.tied()
		    ? FormatRuntime(_("tied: {:s} at {:d}, no points"), JoinRows(table, entry.rows, ", "), entry.count)
		    : FormatRuntime(_("{:s}, {:d}, +{:d}"), RowLabel(table, entry.rows[0]), entry.count, entry.points);
		lines.push_back({ std::string(PlaceName(entry.place)), std::move(text) });
	}
}

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
	lines.push_back({ "", std::string(_("banked + families + awards = total")), UiFlags::ColorBlue });
	for (const uint8_t row : order) {
		const score::RowTotal &t = totals[row];
		lines.push_back({ StrCat(t.place + 1, ". ", RowLabel(table, row)), StrCat(t.banked, " + ", t.families, " + ", t.awards, " = ", t.total) });
	}
}

/** Every player's counts per family (final Families page). */
void AppendFamilyCounts(std::vector<BoardLine> &lines, const ScoreTable &table)
{
	const std::vector<score::FamilyLead> leads = score::FamilyLeads(table, ScoreFamilyOrder());
	if (leads.empty())
		lines.push_back({ "", std::string(_("No family was hunted.")) });
	for (const score::FamilyLead &lead : leads) {
		std::string text;
		for (size_t row = 0; row < table.rows.size(); row++) {
			const uint16_t count = lead.family < table.rows[row].family.size() ? table.rows[row].family[lead.family] : 0;
			if (count == 0)
				continue;
			StrAppend(text, text.empty() ? "" : "   ", table.rows[row].name, " ", count);
		}
		lines.push_back({ std::string(ScoreFamilyName(lead.family)), std::move(text) });
	}
}

/** Every player's count per award (final Awards page). */
void AppendAwardCounts(std::vector<BoardLine> &lines, const ScoreTable &table)
{
	for (size_t a = 0; a < score::NumAwards; a++) {
		const auto award = static_cast<Award>(a);
		std::string text;
		for (const score::ScoreRow &row : table.rows)
			StrAppend(text, text.empty() ? "" : "   ", row.name, " ", score::AwardCount(row, award));
		lines.push_back({ std::string(AwardName(award)), std::move(text) });
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
		lines.push_back({ "", std::string(_("Most kills in a monster family:")), UiFlags::ColorBlue });
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

/** "Anna 12   Bo 9" under a reveal step: each total counting up as the steps reveal points. */
std::string RunningTotalsText(const ScoreTable &table, const std::vector<RevealStep> &steps)
{
	const std::vector<int32_t> running = score::RunningTotals(table, ScoreFamilyOrder(), steps, RevealIndex + 1);
	std::string out;
	for (size_t row = 0; row < table.rows.size(); row++)
		StrAppend(out, out.empty() ? "" : "    ", table.rows[row].name, " ", running[row]);
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
	LivePage = Page::Standings;
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
	case 3: {
		const int step = direction == 2 ? -1 : 1;
		if (ended)
			EndPage = static_cast<FinalPage>((static_cast<int>(EndPage) + 3 + step) % 3);
		else
			LivePage = LivePage == Page::Standings ? Page::Families : Page::Standings;
		SkipLines = 0;
	} break;
	default:
		break;
	}
}

void DrawScoreBoard(const Surface &out)
{
	const ScoreTable *table = ScoreTableIfAny();
	DrawSTextHelp();
	DrawQTextBack(out);

	const Point uiPosition = GetUIRectangle().position;
	const int lineHeight = LineHeight();
	const int sx = uiPosition.x + PaddingLeft;
	const int sy = uiPosition.y;

	std::string title;
	std::vector<BoardLine> lines;
	std::string footer;
	std::string totalsLine;
	if (table == nullptr) {
		title = _("Score Board");
		lines.push_back({ "", std::string(_("The score table has not arrived yet.")) });
	} else if (table->ended == EndReason::None) {
		title = LivePage == Page::Standings ? _("Score Board: places") : _("Score Board: family leads");
		lines = LiveLines(*table);
		footer = _("Left/right: page. Counts are revealed when the game ends.");
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

	DrawString(out, title, { { sx, sy + PaddingTop + 12 }, { ContentTextWidth, lineHeight } }, { .flags = UiFlags::ColorWhitegold | UiFlags::AlignCenter });
	const int titleBottom = sy + HeaderHeight();
	DrawSLine(out, titleBottom);

	int visible = NumVisibleLines();
	int contentY = titleBottom + 6 + 12;
	if (!totalsLine.empty()) {
		// The totals counting up go at the top of the box: the chat lines of the end are drawn over its bottom.
		DrawString(out, totalsLine, { { sx, contentY }, { ContentTextWidth, lineHeight } }, { .flags = UiFlags::ColorGold | UiFlags::AlignCenter });
		contentY += lineHeight + (lineHeight / 2);
		visible -= 2;
	}
	const size_t maxSkip = lines.size() > static_cast<size_t>(visible) ? lines.size() - visible : 0;
	SkipLines = std::min(SkipLines, maxSkip);
	for (int i = 0; i < visible; i++) {
		const size_t index = i + SkipLines;
		if (index >= lines.size())
			break;
		const BoardLine &line = lines[index];
		const int y = contentY + (i * lineHeight);
		if (line.label.empty()) {
			DrawString(out, line.text, { { sx, y }, { ContentTextWidth, lineHeight } }, { .flags = line.color });
		} else {
			DrawString(out, line.label, { { sx, y }, { LabelWidth, lineHeight } }, { .flags = UiFlags::ColorWhitegold });
			DrawString(out, line.text, { { sx + LabelWidth, y }, { ContentTextWidth - LabelWidth, lineHeight } }, { .flags = line.color });
		}
	}
	const int footerY = sy + PaddingTop + PanelHeight - 12 - lineHeight;
	if (!footer.empty())
		DrawString(out, footer, { { sx, footerY }, { ContentTextWidth, lineHeight } }, { .flags = UiFlags::ColorWhitegold | UiFlags::AlignCenter });
}

} // namespace devilution
