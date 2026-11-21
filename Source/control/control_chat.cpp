#include "control_chat.hpp"
#include "control.hpp"
#include "control_panel.hpp"

#include <algorithm>
#include <optional>
#include <vector>

#include "control/control_chat_commands.hpp"
#include "engine/backbuffer_state.hpp"
#include "engine/palette.h"
#include "engine/player_colors.hpp"
#include "engine/rectangle.hpp"
#include "engine/render/clx_render.hpp"
#include "engine/render/primitive_render.hpp"
#include "engine/size.hpp"
#include "options.h"
#include "panels/console.hpp"
#include "panels/mainpanel.hpp"
#include "quick_messages.hpp"
#include "utils/display.h"
#include "utils/sdl_compat.h"
#include "utils/str_cat.hpp"

namespace devilution {

std::optional<TextInputState> ChatInputState;
char TalkMessage[MAX_SEND_STR_LEN];
bool TalkButtonsDown[MAX_PLRS - 1];
int sgbPlrTalkTbl;
bool WhisperList[MAX_PLRS];
OptionalOwnedClxSpriteList talkButtons;

namespace {

char TalkSave[8][MAX_SEND_STR_LEN];
uint8_t TalkSaveIndex;
uint8_t NextTalkSave;
TextInputCursorState ChatCursor;

/** The panel art has three voice (mute) buttons, rows 0-2 of the chat area. */
constexpr int PanelMuteRows = 3;
constexpr int MuteRowHeight = 18;
constexpr Size MuteButtonSize { 61, 16 };

/**
 * Compact rows: with players in slots the three buttons cannot show, every other player in the game gets a row in the
 * same chat area (panel x 172-470, y 65-118): two columns of four 13-pixel rows, a small voice box in the player's
 * colour (filled: voice on, hollow: muted) and the name in the 12-pixel font.
 */
constexpr int CompactRows = 4;
constexpr int CompactRowHeight = 13;
constexpr int CompactColumnWidth = 148;
constexpr Displacement CompactOrigin { 174, 66 };
constexpr int CompactBoxSize = 11;

struct MuteLayout {
	/** The player each row stands for. */
	std::vector<size_t> players;
	bool compact;
};

/**
 * @brief The original layout (rows 0-2 = the other player slots in id order, an empty slot keeping its row) while every
 * player in the game fits in it; otherwise the compact layout with one row per other player in the game.
 */
MuteLayout GetMuteLayout()
{
	std::vector<size_t> others;
	for (size_t i = 0; i < Players.size(); i++) {
		if (&Players[i] != MyPlayer)
			others.push_back(i);
	}
	bool compact = false;
	for (size_t row = PanelMuteRows; row < others.size(); row++)
		compact = compact || Players[others[row]].plractive;
	if (!compact) {
		others.resize(std::min<size_t>(others.size(), PanelMuteRows));
		return { std::move(others), false };
	}
	std::vector<size_t> active;
	for (const size_t i : others) {
		if (Players[i].plractive)
			active.push_back(i);
	}
	return { std::move(active), true };
}

/** @brief The clickable area of a row (the button in the original layout, box and name in the compact one). */
Rectangle MuteRowRect(const MuteLayout &layout, int row)
{
	const Point mainPanelPosition = GetMainPanel().position;
	if (!layout.compact)
		return { mainPanelPosition + Displacement { 172, 69 + (MuteRowHeight * row) }, MuteButtonSize };
	const int column = row / CompactRows;
	const int line = row % CompactRows;
	return { mainPanelPosition + CompactOrigin + Displacement { column * CompactColumnWidth, line * CompactRowHeight },
		{ CompactColumnWidth - 4, CompactRowHeight - 1 } };
}

std::optional<int> MuteRowAt(const MuteLayout &layout, Point mouse)
{
	for (int row = 0; row < static_cast<int>(layout.players.size()); row++) {
		if (MuteRowRect(layout, row).contains(mouse))
			return row;
	}
	return std::nullopt;
}

void DrawCompactMuteRows(const Surface &out, const MuteLayout &layout)
{
	const Point mainPanelPosition = GetMainPanel().position;
	// Cover the three voice buttons of the panel art.
	FillRect(out, mainPanelPosition.x + 172, mainPanelPosition.y + 65, 298, 54, 0);
	for (int row = 0; row < static_cast<int>(layout.players.size()); row++) {
		const size_t id = layout.players[row];
		const Player &player = Players[id];
		const Rectangle rect = MuteRowRect(layout, row);
		const uint8_t playerColor = id < PlayerMapColors.size() ? PlayerMapColors[id] : PlayerMapColors[0];
		const int boxX = rect.position.x;
		const int boxY = rect.position.y + 1;
		FillRect(out, boxX, boxY, CompactBoxSize, CompactBoxSize, playerColor);
		if (!WhisperList[id] || TalkButtonsDown[row]) {
			// muted: hollow; held: a grey middle
			FillRect(out, boxX + 2, boxY + 2, CompactBoxSize - 4, CompactBoxSize - 4, TalkButtonsDown[row] ? PAL16_GRAY + 8 : 0);
		}
		const UiFlags color = player.friendlyMode ? UiFlags::ColorWhitegold : UiFlags::ColorRed;
		const int nameX = boxX + CompactBoxSize + 4;
		DrawString(out, player._pName, { { nameX, rect.position.y }, { rect.size.width - CompactBoxSize - 4, 0 } }, { .flags = color });
		if (!WhisperList[id]) {
			// muted: hollow box and the name struck through
			const int width = std::min(GetLineWidth(player._pName), rect.size.width - CompactBoxSize - 4);
			DrawHorizontalLine(out, { nameX, boxY + (CompactBoxSize / 2) }, width, PAL16_GRAY + 4);
		}
	}
}

void ResetChatMessage()
{
	if (CheckChatCommand(TalkMessage))
		return;

	uint32_t pmask = 0;

	for (size_t i = 0; i < Players.size(); i++) {
		if (WhisperList[i])
			pmask |= 1 << i;
	}

	NetSendCmdString(pmask, TalkMessage);
}

void ControlPressEnter()
{
	if (TalkMessage[0] != 0) {
		ResetChatMessage();
		uint8_t i = 0;
		for (; i < 8; i++) {
			if (strcmp(TalkSave[i], TalkMessage) == 0)
				break;
		}
		if (i >= 8) {
			strcpy(TalkSave[NextTalkSave], TalkMessage);
			NextTalkSave++;
			NextTalkSave &= 7;
		} else {
			uint8_t talkSave = NextTalkSave - 1;
			talkSave &= 7;
			if (i != talkSave) {
				strcpy(TalkSave[i], TalkSave[talkSave]);
				*BufCopy(TalkSave[talkSave], ChatInputState->value()) = '\0';
			}
		}
		TalkMessage[0] = '\0';
		TalkSaveIndex = NextTalkSave;
	}
	ResetChat();
}

void ControlUpDown(int v)
{
	for (int i = 0; i < 8; i++) {
		TalkSaveIndex = (v + TalkSaveIndex) & 7;
		if (TalkSave[TalkSaveIndex][0] != 0) {
			ChatInputState->assign(TalkSave[TalkSaveIndex]);
			return;
		}
	}
}

} // namespace

void DrawChatBox(const Surface &out)
{
	if (!ChatFlag)
		return;

	const Point mainPanelPosition = GetMainPanel().position;

	DrawPanelBox(out, MakeSdlRect(175, sgbPlrTalkTbl + 20, 294, 5), mainPanelPosition + Displacement { 175, 4 });
	int off = 0;
	for (int i = 293; i > 283; off++, i--) {
		DrawPanelBox(out, MakeSdlRect((off / 2) + 175, sgbPlrTalkTbl + off + 25, i, 1), mainPanelPosition + Displacement { (off / 2) + 175, off + 9 });
	}
	DrawPanelBox(out, MakeSdlRect(185, sgbPlrTalkTbl + 35, 274, 30), mainPanelPosition + Displacement { 185, 19 });
	DrawPanelBox(out, MakeSdlRect(180, sgbPlrTalkTbl + 65, 284, 5), mainPanelPosition + Displacement { 180, 49 });
	for (int i = 0; i < 10; i++) {
		DrawPanelBox(out, MakeSdlRect(180, sgbPlrTalkTbl + i + 70, i + 284, 1), mainPanelPosition + Displacement { 180, i + 54 });
	}
	DrawPanelBox(out, MakeSdlRect(170, sgbPlrTalkTbl + 80, 310, 55), mainPanelPosition + Displacement { 170, 64 });

	int x = mainPanelPosition.x + 200;
	const int y = mainPanelPosition.y + 10;

	const uint32_t len = DrawString(out, TalkMessage, { { x, y }, { 250, 39 } },
	    {
	        .flags = UiFlags::ColorWhite | UiFlags::PentaCursor,
	        .lineHeight = 13,
	        .cursorPosition = static_cast<int>(ChatCursor.position),
	        .highlightRange = { static_cast<int>(ChatCursor.selection.begin), static_cast<int>(ChatCursor.selection.end) },
	    });
	ChatInputState->truncate(len);

	const MuteLayout layout = GetMuteLayout();
	if (layout.compact) {
		DrawCompactMuteRows(out, layout);
		return;
	}
	const std::vector<size_t> &rows = layout.players;
	for (int talkBtn = 0; talkBtn < static_cast<int>(rows.size()); talkBtn++) {
		const size_t i = rows[talkBtn];
		Player &player = Players[i];

		const UiFlags color = player.friendlyMode ? UiFlags::ColorWhitegold : UiFlags::ColorRed;
		const Point talkPanPosition = GetMainPanel().position + Displacement { 172, 84 + (MuteRowHeight * talkBtn) };
		if (WhisperList[i]) {
			// the normal (unpressed) voice button is pre-rendered on the panel, only need to draw over it when the button is held
			if (TalkButtonsDown[talkBtn]) {
				const unsigned spriteIndex = talkBtn == 0 ? 2 : 3; // the first button sprite includes a tip from the devils wing so is different to the rest.
				ClxDraw(out, talkPanPosition, (*talkButtons)[spriteIndex]);

				// Draw the translated string over the top of the default (english) button. This graphic is inset to avoid overlapping the wingtip, letting
				// the first button be treated the same as the other two further down the panel.
				RenderClxSprite(out, (*TalkButton)[2], talkPanPosition + Displacement { 4, -15 });
			}
		} else {
			unsigned spriteIndex = talkBtn == 0 ? 0 : 1; // the first button sprite includes a tip from the devils wing so is different to the rest.
			if (TalkButtonsDown[talkBtn])
				spriteIndex += 4; // held button sprites are at index 4 and 5 (with and without wingtip respectively)
			ClxDraw(out, talkPanPosition, (*talkButtons)[spriteIndex]);

			// Draw the translated string over the top of the default (english) button. This graphic is inset to avoid overlapping the wingtip, letting
			// the first button be treated the same as the other two further down the panel.
			RenderClxSprite(out, (*TalkButton)[TalkButtonsDown[talkBtn] ? 1 : 0], talkPanPosition + Displacement { 4, -15 });
		}
		if (player.plractive) {
			DrawString(out, player._pName, { { talkPanPosition.x + 74, talkPanPosition.y - 14 }, { 204, 0 } }, { .flags = color });
		}
	}
}

bool CheckMuteButton()
{
	if (!ChatFlag)
		return false;

	const std::optional<int> row = MuteRowAt(GetMuteLayout(), MousePosition);
	if (!row)
		return false;

	for (bool &talkButtonDown : TalkButtonsDown) {
		talkButtonDown = false;
	}

	TalkButtonsDown[*row] = true;

	return true;
}

void CheckMuteButtonUp()
{
	if (!ChatFlag)
		return;

	for (bool &talkButtonDown : TalkButtonsDown)
		talkButtonDown = false;

	const MuteLayout layout = GetMuteLayout();
	const std::optional<int> row = MuteRowAt(layout, MousePosition);
	if (!row)
		return;

	WhisperList[layout.players[*row]] = !WhisperList[layout.players[*row]];
}

void TypeChatMessage()
{
	if (!IsChatAvailable())
		return;

	ChatFlag = true;
	TalkMessage[0] = '\0';
	ChatInputState.emplace(TextInputState::Options {
	    .value = TalkMessage,
	    .cursor = &ChatCursor,
	    .maxLength = sizeof(TalkMessage) - 1 });
	for (bool &talkButtonDown : TalkButtonsDown) {
		talkButtonDown = false;
	}
	sgbPlrTalkTbl = GetMainPanel().size.height + PanelPaddingHeight;
	RedrawEverything();
	TalkSaveIndex = NextTalkSave;

	SDL_Rect rect = MakeSdlRect(GetMainPanel().position.x + 200, GetMainPanel().position.y + 22, 0, 27);
	SDL_SetTextInputArea(ghMainWnd, &rect, /*cursor=*/0);
	SDLC_StartTextInput(ghMainWnd);
}

void ResetChat()
{
	ChatFlag = false;
	SDLC_StopTextInput(ghMainWnd);
	ChatCursor = {};
	ChatInputState = std::nullopt;
	sgbPlrTalkTbl = 0;
	RedrawEverything();
}

bool IsChatActive()
{
	if (!IsChatAvailable())
		return false;

	if (!ChatFlag)
		return false;

	return true;
}

bool CheckKeypress(SDL_Keycode vkey)
{
	if (!IsChatAvailable())
		return false;
	if (!ChatFlag)
		return false;

	switch (vkey) {
	case SDLK_ESCAPE:
		ResetChat();
		return true;
	case SDLK_RETURN:
	case SDLK_KP_ENTER:
		ControlPressEnter();
		return true;
	case SDLK_DOWN:
		ControlUpDown(1);
		return true;
	case SDLK_UP:
		ControlUpDown(-1);
		return true;
	default:
		return vkey >= SDLK_SPACE && vkey <= SDLK_Z;
	}
}

void DiabloHotkeyMsg(uint32_t dwMsg)
{
	assert(dwMsg < QuickMessages.size());

#ifdef _DEBUG
	constexpr std::string_view LuaPrefix = "/lua ";
	for (const std::string &msg : GetOptions().Chat.szHotKeyMsgs[dwMsg]) {
		if (!msg.starts_with(LuaPrefix)) continue;
		InitConsole();
		RunInConsole(std::string_view(msg).substr(LuaPrefix.size()));
	}
#endif

	if (!IsChatAvailable()) {
		return;
	}

	for (const std::string &msg : GetOptions().Chat.szHotKeyMsgs[dwMsg]) {
#ifdef _DEBUG
		if (msg.starts_with(LuaPrefix)) continue;
#endif
		char charMsg[MAX_SEND_STR_LEN];
		CopyUtf8(charMsg, msg, sizeof(charMsg));
		NetSendCmdString(0xFFFFFF, charMsg);
	}
}

bool IsChatAvailable()
{
	return gbIsMultiplayer;
}

bool HandleTalkTextInputEvent(const SDL_Event &event)
{
	return HandleInputEvent(event, ChatInputState);
}

} // namespace devilution
