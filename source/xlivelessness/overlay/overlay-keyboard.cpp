#include <winsock2.h>
#include <Windows.h>
#include "overlay-keyboard.hpp"
#include "overlay.hpp"
#include "overlay-widgets.hpp"
#include "overlay-gamepad.hpp"
#include "../xlive/xdefs.hpp"
#include "../xlive/xlive.hpp"
#include "../xlln/debug-log.hpp"
#include <imgui.h>
#include <atomic>
#include <string>
#include <vector>
#include <string.h>
#include <math.h>

typedef struct _OVERLAY_KEYBOARD_REQUEST {
	uint32_t flags = 0;
	std::wstring titleText;
	std::wstring descriptionText;
	std::wstring defaultText;
	wchar_t* resultText = 0;
	size_t resultTextSize = 0;
	XOVERLAPPED* xoverlapped = 0;
} OVERLAY_KEYBOARD_REQUEST;

static CRITICAL_SECTION xlln_critsec_overlay_keyboard;

// Handed over by the thread the Title calls XShowKeyboardUI from, taken by the render thread.
static OVERLAY_KEYBOARD_REQUEST overlay_keyboard_incoming;
static bool overlay_keyboard_incoming_valid = false;
// The request the modal is showing. Render thread only.
static OVERLAY_KEYBOARD_REQUEST overlay_keyboard_active;
static bool overlay_keyboard_active_valid = false;
// Answered by the frame, handed to the Title once the frame lock is released.
static XOVERLAPPED* overlay_keyboard_completed = 0;
static uint32_t overlay_keyboard_completed_code = 0;

static std::atomic<bool> overlay_keyboard_open(false);
static std::atomic<bool> overlay_keyboard_text_focus(false);

// What ImGui::InputText edits, in UTF-8. Render thread only.
static std::vector<char> overlay_keyboard_buffer;
static bool overlay_keyboard_take_focus = false;
static bool overlay_keyboard_focus_grid = false;
static bool overlay_keyboard_focus_done = false;

// Caret and selection as byte offsets. The widget reports its own through the callback while
// active; the grid edits at these while it is not, because an inactive widget takes no text.
static int overlay_keyboard_caret = 0;
static int overlay_keyboard_selection_start = 0;
static int overlay_keyboard_selection_end = 0;
static bool overlay_keyboard_field_active = false;

enum class OverlayKeyboardPage : uint32_t {
	LETTERS = 0,
	SYMBOLS,
	ACCENTS,
};
static OverlayKeyboardPage overlay_keyboard_page = OverlayKeyboardPage::LETTERS;
static bool overlay_keyboard_shift = false;
// When the next repeat of a held X fires. Below zero while X is up.
static double overlay_keyboard_backspace_next_repeat = -1.0;

// The overlay font atlas carries Latin-1 and a little punctuation. A request for a character set
// with no glyphs in it would draw a field of empty boxes, so it goes to the desktop window.
#define OVERLAY_KEYBOARD_SETS_WITHOUT_GLYPHS ( \
	VKBD_JAPANESE_FULL | VKBD_KOREAN_FULL | VKBD_TCH_FULL | VKBD_RUSSIAN_FULL | VKBD_SCH_FULL \
	| VKBD_JAPANESE_HIRAGANA | VKBD_JAPANESE_KATAKANA | VKBD_GREEK_FULL | VKBD_CSH_FULL \
)

// A request for digits only gets a number pad instead of the full grid.
#define OVERLAY_KEYBOARD_SETS_NUMERIC (VKBD_LATIN_NUMERIC | VKBD_LATIN_PHONE | VKBD_LATIN_IP_ADDRESS)

#define OVERLAY_KEYBOARD_ROWS 4
#define OVERLAY_KEYBOARD_COLUMNS 10

// The ten middle keys of each row; the side keys are the same on every page and drawn by the grid.
static const char* const overlay_keyboard_letters_lower[OVERLAY_KEYBOARD_ROWS][OVERLAY_KEYBOARD_COLUMNS] = {
	{ "1", "2", "3", "4", "5", "6", "7", "8", "9", "0" },
	{ "q", "w", "e", "r", "t", "y", "u", "i", "o", "p" },
	{ "a", "s", "d", "f", "g", "h", "j", "k", "l", "'" },
	{ "z", "x", "c", "v", "b", "n", "m", ",", ".", "?" },
};
static const char* const overlay_keyboard_letters_upper[OVERLAY_KEYBOARD_ROWS][OVERLAY_KEYBOARD_COLUMNS] = {
	{ "1", "2", "3", "4", "5", "6", "7", "8", "9", "0" },
	{ "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P" },
	{ "A", "S", "D", "F", "G", "H", "J", "K", "L", "\"" },
	{ "Z", "X", "C", "V", "B", "N", "M", ";", ":", "!" },
};
static const char* const overlay_keyboard_symbols[OVERLAY_KEYBOARD_ROWS][OVERLAY_KEYBOARD_COLUMNS] = {
	{ "!", "@", "#", "$", "%", "^", "&", "*", "(", ")" },
	{ "-", "_", "=", "+", "[", "]", "{", "}", "\\", "|" },
	{ ";", ":", "'", "\"", ",", ".", "<", ">", "/", "?" },
	{ "~", "`", "\xC2\xA3", "\xC2\xA5", "\xC2\xA7", "\xC2\xA9", "\xC2\xAE", "\xC2\xB0", "\xC2\xB1", "\xC2\xB5" },
};
// Y with diaeresis and sharp s stay lower case on the upper page: no capital in the font atlas.
static const char* const overlay_keyboard_accents_lower[OVERLAY_KEYBOARD_ROWS][OVERLAY_KEYBOARD_COLUMNS] = {
	{ "\xC3\xA0", "\xC3\xA1", "\xC3\xA2", "\xC3\xA3", "\xC3\xA4", "\xC3\xA5", "\xC3\xA6", "\xC3\xA7", "\xC3\xA8", "\xC3\xA9" },
	{ "\xC3\xAA", "\xC3\xAB", "\xC3\xAC", "\xC3\xAD", "\xC3\xAE", "\xC3\xAF", "\xC3\xB0", "\xC3\xB1", "\xC3\xB2", "\xC3\xB3" },
	{ "\xC3\xB4", "\xC3\xB5", "\xC3\xB6", "\xC3\xB8", "\xC3\xB9", "\xC3\xBA", "\xC3\xBB", "\xC3\xBC", "\xC3\xBD", "\xC3\xBE" },
	{ "\xC3\xBF", "\xC3\x9F", "\xC2\xBF", "\xC2\xA1", "\xC2\xAB", "\xC2\xBB", "\xC2\xAA", "\xC2\xBA", "\xC2\xAC", "\xC2\xA2" },
};
static const char* const overlay_keyboard_accents_upper[OVERLAY_KEYBOARD_ROWS][OVERLAY_KEYBOARD_COLUMNS] = {
	{ "\xC3\x80", "\xC3\x81", "\xC3\x82", "\xC3\x83", "\xC3\x84", "\xC3\x85", "\xC3\x86", "\xC3\x87", "\xC3\x88", "\xC3\x89" },
	{ "\xC3\x8A", "\xC3\x8B", "\xC3\x8C", "\xC3\x8D", "\xC3\x8E", "\xC3\x8F", "\xC3\x90", "\xC3\x91", "\xC3\x92", "\xC3\x93" },
	{ "\xC3\x94", "\xC3\x95", "\xC3\x96", "\xC3\x98", "\xC3\x99", "\xC3\x9A", "\xC3\x9B", "\xC3\x9C", "\xC3\x9D", "\xC3\x9E" },
	{ "\xC3\xBF", "\xC3\x9F", "\xC2\xBF", "\xC2\xA1", "\xC2\xAB", "\xC2\xBB", "\xC2\xAA", "\xC2\xBA", "\xC2\xAC", "\xC2\xA2" },
};
// An address needs @ and _ on the first page, in place of two keys an address never holds.
static const char* const overlay_keyboard_email_lower[3] = { "@", ".", "_" };
static const char* const overlay_keyboard_email_upper[3] = { "@", ".", "-" };

static const char* const overlay_keyboard_digits[3][3] = {
	{ "1", "2", "3" },
	{ "4", "5", "6" },
	{ "7", "8", "9" },
};

void InitOverlayKeyboard()
{
	InitializeCriticalSection(&xlln_critsec_overlay_keyboard);
}

void UninitOverlayKeyboard()
{
	// The completion is dropped instead of run. It calls a routine inside the Title, and by this
	// point the Title is already going away.
	OverlayKeyboardCancelActive();
	EnterCriticalSection(&xlln_critsec_overlay_keyboard);
	overlay_keyboard_completed = 0;
	overlay_keyboard_incoming = OVERLAY_KEYBOARD_REQUEST();
	overlay_keyboard_incoming_valid = false;
	LeaveCriticalSection(&xlln_critsec_overlay_keyboard);
	DeleteCriticalSection(&xlln_critsec_overlay_keyboard);
}

bool OverlayKeyboardIsOpen()
{
	return overlay_keyboard_open.load();
}

bool OverlayKeyboardHoldsTextFocus()
{
	return overlay_keyboard_text_focus.load();
}

static std::string OverlayKeyboardToUtf8(const std::wstring &text)
{
	if (text.empty()) {
		return std::string();
	}
	int byteCount = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), 0, 0, 0, 0);
	if (byteCount <= 0) {
		return std::string();
	}
	std::string result((size_t)byteCount, '\0');
	WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &result[0], byteCount, 0, 0);
	return result;
}

bool OverlayKeyboardRequest(
	uint32_t flags
	, const wchar_t* default_text
	, const wchar_t* title_text
	, const wchar_t* description_text
	, wchar_t* result_text
	, size_t result_text_size
	, XOVERLAPPED* xoverlapped
)
{
	if (!OverlayIsAvailable() || OverlayProviderIsActive()) {
		return false;
	}
	if (flags & OVERLAY_KEYBOARD_SETS_WITHOUT_GLYPHS) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_INFO
			, "%s the Title asked for a character set (0x%08x) the overlay has no glyphs for, so the desktop window answers instead."
			, __func__
			, flags
		);
		return false;
	}

	EnterCriticalSection(&xlln_critsec_overlay_keyboard);
	overlay_keyboard_incoming.flags = flags;
	overlay_keyboard_incoming.titleText = title_text ? title_text : L"";
	overlay_keyboard_incoming.descriptionText = description_text ? description_text : L"";
	overlay_keyboard_incoming.defaultText = default_text ? default_text : L"";
	overlay_keyboard_incoming.resultText = result_text;
	overlay_keyboard_incoming.resultTextSize = result_text_size;
	overlay_keyboard_incoming.xoverlapped = xoverlapped;
	overlay_keyboard_incoming_valid = true;
	LeaveCriticalSection(&xlln_critsec_overlay_keyboard);

	overlay_keyboard_open.store(true);
	OverlayGuideRequestOpenKeyboard();

	return true;
}

// Writes the answer into the Title's buffer and queues the completion. The XOVERLAPPED itself is
// not touched here: the frame lock is held, and completing it runs the Title's completion routine.
static void OverlayKeyboardFinish(uint32_t error_code)
{
	if (!overlay_keyboard_active_valid) {
		return;
	}

	if (error_code == ERROR_SUCCESS && overlay_keyboard_active.resultText && overlay_keyboard_active.resultTextSize) {
		int written = MultiByteToWideChar(
			CP_UTF8
			, 0
			, overlay_keyboard_buffer.data()
			, -1
			, overlay_keyboard_active.resultText
			, (int)overlay_keyboard_active.resultTextSize
		);
		if (written <= 0) {
			overlay_keyboard_active.resultText[0] = 0;
		}
		overlay_keyboard_active.resultText[overlay_keyboard_active.resultTextSize - 1] = 0;
	}

	EnterCriticalSection(&xlln_critsec_overlay_keyboard);
	overlay_keyboard_completed = overlay_keyboard_active.xoverlapped;
	overlay_keyboard_completed_code = error_code;
	LeaveCriticalSection(&xlln_critsec_overlay_keyboard);

	overlay_keyboard_active = OVERLAY_KEYBOARD_REQUEST();
	overlay_keyboard_active_valid = false;
	overlay_keyboard_open.store(false);
	overlay_keyboard_text_focus.store(false);
	overlay_keyboard_buffer.clear();
	overlay_keyboard_caret = 0;
	overlay_keyboard_selection_start = 0;
	overlay_keyboard_selection_end = 0;
	overlay_keyboard_field_active = false;
	overlay_keyboard_backspace_next_repeat = -1.0;
}

void OverlayKeyboardCancelActive()
{
	OverlayKeyboardFinish(ERROR_CANCELLED);
}

void OverlayKeyboardFlushCompletion()
{
	XOVERLAPPED* xoverlapped = 0;
	uint32_t errorCode = ERROR_SUCCESS;

	EnterCriticalSection(&xlln_critsec_overlay_keyboard);
	xoverlapped = overlay_keyboard_completed;
	errorCode = overlay_keyboard_completed_code;
	overlay_keyboard_completed = 0;
	LeaveCriticalSection(&xlln_critsec_overlay_keyboard);

	if (!xoverlapped) {
		return;
	}

	xoverlapped->InternalLow = errorCode;
	xoverlapped->InternalHigh = errorCode;
	xoverlapped->dwExtendedError = errorCode;

	Check_Overlapped_Inline(xoverlapped);
}

// Moves a handed-over request onto the render thread. A second request replaces the first, which
// is answered as cancelled so the Title waiting on it is never left without a reply.
static void OverlayKeyboardTakeIncoming()
{
	EnterCriticalSection(&xlln_critsec_overlay_keyboard);
	bool hasIncoming = overlay_keyboard_incoming_valid;
	OVERLAY_KEYBOARD_REQUEST incoming;
	if (hasIncoming) {
		incoming = overlay_keyboard_incoming;
		overlay_keyboard_incoming = OVERLAY_KEYBOARD_REQUEST();
		overlay_keyboard_incoming_valid = false;
	}
	LeaveCriticalSection(&xlln_critsec_overlay_keyboard);

	if (!hasIncoming) {
		return;
	}
	if (overlay_keyboard_active_valid) {
		OverlayKeyboardFinish(ERROR_CANCELLED);
		OverlayKeyboardFlushCompletion();
	}

	overlay_keyboard_active = incoming;
	overlay_keyboard_active_valid = true;
	overlay_keyboard_open.store(true);

	// A pad starts on the grid, a keyboard in the field: an active field keeps the d-pad for its caret,
	// so a pad could not leave it. VKBD_SELECT_OK starts the cursor on Done.
	bool padConnected = OverlayGamepadIsConnected();
	overlay_keyboard_focus_done = padConnected && (overlay_keyboard_active.flags & VKBD_SELECT_OK) != 0;
	overlay_keyboard_focus_grid = padConnected && !overlay_keyboard_focus_done;
	overlay_keyboard_take_focus = !padConnected;
	overlay_keyboard_page = OverlayKeyboardPage::LETTERS;
	overlay_keyboard_shift = false;
	overlay_keyboard_backspace_next_repeat = -1.0;

	// A buffer of cchResultText bytes can never hold more than cchResultText - 1 UTF-16 units,
	// because no character takes fewer UTF-8 bytes than it takes UTF-16 units. So the edited text
	// always fits the Title's buffer, and the character limit needs no separate check.
	overlay_keyboard_buffer.assign(overlay_keyboard_active.resultTextSize, '\0');
	std::string defaultUtf8 = OverlayKeyboardToUtf8(overlay_keyboard_active.defaultText);
	if (defaultUtf8.size() >= overlay_keyboard_buffer.size()) {
		defaultUtf8.resize(overlay_keyboard_buffer.size() - 1);
	}
	memcpy(overlay_keyboard_buffer.data(), defaultUtf8.c_str(), defaultUtf8.size());

	// VKBD_HIGHLIGHT_TEXT selects the existing text, so the first key replaces it.
	overlay_keyboard_caret = (int)defaultUtf8.size();
	overlay_keyboard_selection_start = (overlay_keyboard_active.flags & VKBD_HIGHLIGHT_TEXT) ? 0 : overlay_keyboard_caret;
	overlay_keyboard_selection_end = overlay_keyboard_caret;
}

// --- editing the buffer from the grid ---------------------------------------------------------

static int OverlayKeyboardTextLength()
{
	if (overlay_keyboard_buffer.empty()) {
		return 0;
	}
	return (int)strnlen(overlay_keyboard_buffer.data(), overlay_keyboard_buffer.size());
}

static bool OverlayKeyboardIsCodePointStart(char byte)
{
	return ((unsigned char)byte & 0xC0) != 0x80;
}

static int OverlayKeyboardPreviousCodePoint(int offset)
{
	offset--;
	while (offset > 0 && !OverlayKeyboardIsCodePointStart(overlay_keyboard_buffer[offset])) {
		offset--;
	}
	return offset < 0 ? 0 : offset;
}

static int OverlayKeyboardNextCodePoint(int offset, int length)
{
	offset++;
	while (offset < length && !OverlayKeyboardIsCodePointStart(overlay_keyboard_buffer[offset])) {
		offset++;
	}
	return offset > length ? length : offset;
}

static void OverlayKeyboardDeleteRange(int start, int end)
{
	int length = OverlayKeyboardTextLength();
	if (start < 0) {
		start = 0;
	}
	if (end > length) {
		end = length;
	}
	if (start >= end) {
		return;
	}
	memmove(&overlay_keyboard_buffer[start], &overlay_keyboard_buffer[end], (size_t)(length - end + 1));
	overlay_keyboard_caret = start;
	overlay_keyboard_selection_start = start;
	overlay_keyboard_selection_end = start;
}

static void OverlayKeyboardDeleteSelection()
{
	int start = overlay_keyboard_selection_start;
	int end = overlay_keyboard_selection_end;
	if (start > end) {
		int swap = start;
		start = end;
		end = swap;
	}
	OverlayKeyboardDeleteRange(start, end);
}

static void OverlayKeyboardInsert(const char* utf8)
{
	OverlayKeyboardDeleteSelection();
	int insertLength = (int)strlen(utf8);
	int length = OverlayKeyboardTextLength();
	// Insert the whole character or nothing; a cut sequence would hand the Title broken UTF-8.
	if (length + insertLength >= (int)overlay_keyboard_buffer.size()) {
		return;
	}
	if (overlay_keyboard_caret < 0) {
		overlay_keyboard_caret = 0;
	}
	if (overlay_keyboard_caret > length) {
		overlay_keyboard_caret = length;
	}
	memmove(
		&overlay_keyboard_buffer[overlay_keyboard_caret + insertLength]
		, &overlay_keyboard_buffer[overlay_keyboard_caret]
		, (size_t)(length - overlay_keyboard_caret + 1)
	);
	memcpy(&overlay_keyboard_buffer[overlay_keyboard_caret], utf8, (size_t)insertLength);
	overlay_keyboard_caret += insertLength;
	overlay_keyboard_selection_start = overlay_keyboard_caret;
	overlay_keyboard_selection_end = overlay_keyboard_caret;
}

static void OverlayKeyboardBackspace()
{
	if (overlay_keyboard_selection_start != overlay_keyboard_selection_end) {
		OverlayKeyboardDeleteSelection();
		return;
	}
	if (overlay_keyboard_caret <= 0) {
		return;
	}
	OverlayKeyboardDeleteRange(OverlayKeyboardPreviousCodePoint(overlay_keyboard_caret), overlay_keyboard_caret);
}

static void OverlayKeyboardMoveCaret(int direction)
{
	int length = OverlayKeyboardTextLength();
	if (overlay_keyboard_caret > length) {
		overlay_keyboard_caret = length;
	}
	overlay_keyboard_caret = direction < 0
		? OverlayKeyboardPreviousCodePoint(overlay_keyboard_caret)
		: OverlayKeyboardNextCodePoint(overlay_keyboard_caret, length);
	overlay_keyboard_selection_start = overlay_keyboard_caret;
	overlay_keyboard_selection_end = overlay_keyboard_caret;
}

static void OverlayKeyboardNextPage()
{
	switch (overlay_keyboard_page) {
		case OverlayKeyboardPage::LETTERS: overlay_keyboard_page = OverlayKeyboardPage::SYMBOLS; break;
		case OverlayKeyboardPage::SYMBOLS: overlay_keyboard_page = OverlayKeyboardPage::ACCENTS; break;
		default: overlay_keyboard_page = OverlayKeyboardPage::LETTERS; break;
	}
}

// Records the widget's caret, so the grid carries on from it once the widget goes inactive.
static int OverlayKeyboardFieldCallback(ImGuiInputTextCallbackData* data)
{
	if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways) {
		overlay_keyboard_caret = data->CursorPos;
		overlay_keyboard_selection_start = data->SelectionStart;
		overlay_keyboard_selection_end = data->SelectionEnd;
	}
	return 0;
}

// --- drawing ------------------------------------------------------------------------------------

// An inactive widget draws no caret or selection, and the grid only works on an inactive one, so
// both are drawn here from the grid's offsets.
static void OverlayKeyboardDrawCaret(ImFont* font, ImVec2 frame_padding, bool is_password, bool is_multiline)
{
	ImVec2 fieldMin = ImGui::GetItemRectMin();
	ImVec2 fieldMax = ImGui::GetItemRectMax();
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	int length = OverlayKeyboardTextLength();

	// Text up to an offset, as the widget shows it: a masked field shows one star per character.
	auto displayPrefix = [&](int offset) -> std::string {
		if (offset > length) {
			offset = length;
		}
		if (offset < 0) {
			offset = 0;
		}
		if (!is_password) {
			return std::string(overlay_keyboard_buffer.data(), (size_t)offset);
		}
		size_t characters = 0;
		for (int iByte = 0; iByte < offset; iByte++) {
			if (OverlayKeyboardIsCodePointStart(overlay_keyboard_buffer[iByte])) {
				characters++;
			}
		}
		return std::string(characters, '*');
	};
	auto textPosition = [&](const std::string &prefix) -> ImVec2 {
		int line = 0;
		size_t lineStart = 0;
		if (is_multiline) {
			for (size_t iByte = 0; iByte < prefix.size(); iByte++) {
				if (prefix[iByte] == '\n') {
					line++;
					lineStart = iByte + 1;
				}
			}
		}
		float x = fieldMin.x + frame_padding.x + font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.f, prefix.c_str() + lineStart).x;
		float y = fieldMin.y + frame_padding.y + (float)line * font->FontSize;
		float rightEdge = fieldMax.x - frame_padding.x;
		return ImVec2(x > rightEdge ? rightEdge : x, y);
	};

	ImVec2 clipMin(fieldMin.x + frame_padding.x, fieldMin.y);
	ImVec2 clipMax(fieldMax.x - frame_padding.x, fieldMax.y);
	drawList->PushClipRect(clipMin, clipMax, true);

	if (overlay_keyboard_selection_start != overlay_keyboard_selection_end && !is_multiline) {
		int start = overlay_keyboard_selection_start;
		int end = overlay_keyboard_selection_end;
		if (start > end) {
			int swap = start;
			start = end;
			end = swap;
		}
		ImVec2 startPosition = textPosition(displayPrefix(start));
		ImVec2 endPosition = textPosition(displayPrefix(end));
		drawList->AddRectFilled(
			startPosition
			, ImVec2(endPosition.x, startPosition.y + font->FontSize)
			, OverlayAlpha(OverlayColorWithAlpha(OVERLAY_COL_AMBER, 70))
		);
	}

	if (fmod(ImGui::GetTime(), 1.0) < 0.6) {
		ImVec2 caretPosition = textPosition(displayPrefix(overlay_keyboard_caret));
		drawList->AddRectFilled(
			caretPosition
			, ImVec2(caretPosition.x + OverlayPx(1.5f), caretPosition.y + font->FontSize)
			, OverlayAlpha(OVERLAY_COL_AMBER)
		);
	}

	drawList->PopClipRect();
}

// One key of the grid, as a real item so the pad, the arrow keys and the mouse all reach it.
static bool OverlayKeyboardKey(const char* str_id, ImVec2 position, ImVec2 size, const char* label, const char* pad_hint, bool toggled)
{
	ImGui::SetCursorScreenPos(position);
	ImGui::PushStyleColor(ImGuiCol_NavHighlight, ImVec4(0.f, 0.f, 0.f, 0.f));
	bool pressed = ImGui::InvisibleButton(str_id, size, ImGuiButtonFlags_EnableNav);
	ImGui::PopStyleColor();
	bool hovered = ImGui::IsItemHovered();
	bool active = ImGui::IsItemActive();
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	ImVec2 rectMax(position.x + size.x, position.y + size.y);
	float rounding = OverlayPx(7.f);

	ImU32 textColor = overlay_palette.keycapText;
	if (toggled) {
		OverlayDrawAmberFace(drawList, position, rectMax, rounding, OVERLAY_COL_AMBER_HI, OVERLAY_COL_AMBER_LO);
		textColor = OVERLAY_COL_AMBER_INK;
	}
	else {
		// A press darkens the key; hover moves it toward the wash, the colour that differs on every theme.
		ImU32 gradientTop = OVERLAY_COL_KEYCAP_TOP;
		ImU32 gradientBottom = OVERLAY_COL_KEYCAP_BOTTOM;
		if (active) {
			gradientTop = OverlayColorLerp(gradientTop, IM_COL32(0, 0, 0, 255), 0.18f);
			gradientBottom = OverlayColorLerp(gradientBottom, IM_COL32(0, 0, 0, 255), 0.18f);
		}
		else if (hovered) {
			gradientTop = OverlayColorLerp(gradientTop, overlay_palette.wash, 0.08f);
			gradientBottom = OverlayColorLerp(gradientBottom, overlay_palette.wash, 0.08f);
		}
		OverlayDrawRectVGradient(drawList, position, rectMax, gradientTop, gradientBottom, rounding);
		drawList->AddRect(position, rectMax, OverlayAlpha(OVERLAY_COL_BORDER_HI), rounding);
		drawList->AddLine(
			ImVec2(position.x + rounding, rectMax.y - 1.f)
			, ImVec2(rectMax.x - rounding, rectMax.y - 1.f)
			, OverlayAlpha(overlay_palette.keycapLip)
		);
	}

	ImFont* labelFont = overlay_fonts.semi13;
	ImVec2 labelSize = labelFont->CalcTextSizeA(labelFont->FontSize, FLT_MAX, 0.f, label);
	float hintGap = OverlayPx(5.f);
	float hintPadding = OverlayPx(3.f);
	ImFont* hintFont = overlay_fonts.semi11;
	ImVec2 hintSize(0.f, 0.f);
	if (pad_hint) {
		hintSize = hintFont->CalcTextSizeA(hintFont->FontSize, FLT_MAX, 0.f, pad_hint);
		hintSize.x += hintPadding * 2.f;
		hintSize.y += hintPadding;
	}
	// A tall key stacks the hint under the label; a wide key keeps it beside, or drops it when it
	// would not fit.
	ImVec2 center(position.x + size.x * 0.5f, position.y + size.y * 0.5f);
	ImVec2 labelPosition;
	ImVec2 hintMin;
	if (pad_hint && size.y > size.x) {
		float stackGap = OverlayPx(4.f);
		float stackTop = center.y - (labelSize.y + stackGap + hintSize.y) * 0.5f;
		labelPosition = ImVec2(center.x - labelSize.x * 0.5f, stackTop);
		hintMin = ImVec2(center.x - hintSize.x * 0.5f, stackTop + labelSize.y + stackGap);
	}
	else {
		float groupWidth = labelSize.x + (pad_hint ? hintGap + hintSize.x : 0.f);
		if (pad_hint && groupWidth > size.x - OverlayPx(6.f)) {
			pad_hint = 0;
			groupWidth = labelSize.x;
		}
		float groupX = position.x + (size.x - groupWidth) * 0.5f;
		labelPosition = ImVec2(groupX, center.y - labelSize.y * 0.5f);
		hintMin = ImVec2(groupX + labelSize.x + hintGap, center.y - hintSize.y * 0.5f);
	}
	drawList->AddText(labelFont, labelFont->FontSize, labelPosition, OverlayAlpha(textColor), label);
	if (pad_hint) {
		ImVec2 hintMax(hintMin.x + hintSize.x, hintMin.y + hintSize.y);
		ImU32 hintBackground = toggled ? IM_COL32(0, 0, 0, 56) : OVERLAY_COL_WASH_A(26);
		ImU32 hintColor = toggled ? OVERLAY_COL_AMBER_INK : OVERLAY_COL_TEXT_DIM;
		drawList->AddRectFilled(hintMin, hintMax, OverlayAlpha(hintBackground), OverlayPx(4.f));
		drawList->AddText(hintFont, hintFont->FontSize, ImVec2(hintMin.x + hintPadding, hintMin.y + hintPadding * 0.5f), OverlayAlpha(hintColor), pad_hint);
	}

	OverlayFocusRingForLastItem(rounding);
	return pressed;
}

// Puts the navigation cursor on the next key drawn, once. Focusing a key does not press it.
static void OverlayKeyboardFocusNext(bool* pending)
{
	if (*pending) {
		*pending = false;
		ImGui::SetKeyboardFocusHere();
		ImGui::SetNavCursorVisible(true);
	}
}

enum class OverlayKeyboardKeyAction : uint32_t {
	NONE = 0,
	INSERT,
	BACKSPACE,
	CARET_LEFT,
	CARET_RIGHT,
	SHIFT,
	PAGE,
	SPACE,
	DONE,
	NEWLINE,
};

// The page the middle keys are read from, and the label of the key that moves to the next one.
static const char* const (*OverlayKeyboardPageKeys(const char** next_page_label))[OVERLAY_KEYBOARD_COLUMNS]
{
	switch (overlay_keyboard_page) {
		case OverlayKeyboardPage::SYMBOLS: {
			*next_page_label = "\xC3\xA0\xC3\xA9";
			return overlay_keyboard_symbols;
		}
		case OverlayKeyboardPage::ACCENTS: {
			*next_page_label = "abc";
			return overlay_keyboard_shift ? overlay_keyboard_accents_upper : overlay_keyboard_accents_lower;
		}
		default: {
			*next_page_label = "#+=";
			return overlay_keyboard_shift ? overlay_keyboard_letters_upper : overlay_keyboard_letters_lower;
		}
	}
}

// Draws the grid and returns the pressed key's action, with the text an INSERT carries.
static OverlayKeyboardKeyAction OverlayKeyboardBuildGrid(
	ImVec2 grid_origin
	, float key_unit
	, float key_gap
	, bool is_numeric
	, bool is_multiline
	, bool pad_connected
	, const char** out_insert
)
{
	OverlayKeyboardKeyAction action = OverlayKeyboardKeyAction::NONE;
	*out_insert = 0;
	float sideWidth = key_unit * 1.5f;
	float step = key_unit + key_gap;
	char keyId[32];

	auto sideKey = [&](const char* id, float x, float y, float height, const char* label, const char* hint, bool toggled) -> bool {
		return OverlayKeyboardKey(id, ImVec2(x, y), ImVec2(sideWidth, height), label, pad_connected ? hint : 0, toggled);
	};
	if (is_numeric) {
		uint32_t flags = overlay_keyboard_active.flags;
		const char* lastRow[3] = { "-", "0", "." };
		if (flags & VKBD_LATIN_PHONE) {
			lastRow[0] = "+";
			lastRow[2] = "#";
		}
		else if (flags & VKBD_LATIN_IP_ADDRESS) {
			lastRow[0] = ":";
		}
		for (int iRow = 0; iRow < 4; iRow++) {
			float rowY = grid_origin.y + (float)iRow * step;
			for (int iColumn = 0; iColumn < 3; iColumn++) {
				const char* keyText = iRow < 3 ? overlay_keyboard_digits[iRow][iColumn] : lastRow[iColumn];
				snprintf(keyId, sizeof(keyId), "##xlln_key_%d_%d", iRow, iColumn);
				if (iRow == 0 && iColumn == 0) {
					OverlayKeyboardFocusNext(&overlay_keyboard_focus_grid);
				}
				if (OverlayKeyboardKey(keyId, ImVec2(grid_origin.x + (float)iColumn * step, rowY), ImVec2(key_unit, key_unit), keyText, 0, false)) {
					action = OverlayKeyboardKeyAction::INSERT;
					*out_insert = keyText;
				}
			}
		}
		float sideX = grid_origin.x + 3.f * step;
		if (sideKey("##xlln_key_back", sideX, grid_origin.y, key_unit, "Back", "X", false)) {
			action = OverlayKeyboardKeyAction::BACKSPACE;
		}
		if (sideKey("##xlln_key_left", sideX, grid_origin.y + step, key_unit, "\xE2\x86\x90", "LB", false)) {
			action = OverlayKeyboardKeyAction::CARET_LEFT;
		}
		if (sideKey("##xlln_key_right", sideX, grid_origin.y + 2.f * step, key_unit, "\xE2\x86\x92", "RB", false)) {
			action = OverlayKeyboardKeyAction::CARET_RIGHT;
		}
		OverlayKeyboardFocusNext(&overlay_keyboard_focus_done);
		if (sideKey("##xlln_key_done", sideX, grid_origin.y + 3.f * step, key_unit, "Done", "Menu", false)) {
			action = OverlayKeyboardKeyAction::DONE;
		}
		return action;
	}

	const char* nextPageLabel = 0;
	const char* const (*pageKeys)[OVERLAY_KEYBOARD_COLUMNS] = OverlayKeyboardPageKeys(&nextPageLabel);
	bool isEmail = (overlay_keyboard_active.flags & VKBD_LATIN_EMAIL) != 0 && overlay_keyboard_page == OverlayKeyboardPage::LETTERS;
	float middleX = grid_origin.x + sideWidth + key_gap;
	float rightX = middleX + (float)OVERLAY_KEYBOARD_COLUMNS * step;

	for (int iRow = 0; iRow < OVERLAY_KEYBOARD_ROWS; iRow++) {
		float rowY = grid_origin.y + (float)iRow * step;
		for (int iColumn = 0; iColumn < OVERLAY_KEYBOARD_COLUMNS; iColumn++) {
			const char* keyText = pageKeys[iRow][iColumn];
			if (isEmail && iRow == 3 && iColumn >= 7) {
				keyText = overlay_keyboard_shift ? overlay_keyboard_email_upper[iColumn - 7] : overlay_keyboard_email_lower[iColumn - 7];
			}
			snprintf(keyId, sizeof(keyId), "##xlln_key_%d_%d", iRow, iColumn);
			if (iRow == 1 && iColumn == 0) {
				OverlayKeyboardFocusNext(&overlay_keyboard_focus_grid);
			}
			if (OverlayKeyboardKey(keyId, ImVec2(middleX + (float)iColumn * step, rowY), ImVec2(key_unit, key_unit), keyText, 0, false)) {
				action = OverlayKeyboardKeyAction::INSERT;
				*out_insert = keyText;
			}
		}
	}
	// Caret keys alone on the top row; the page key and Back span the two letter rows; Shift and Done
	// span the bottom row and the space bar. Multi-line splits the Done slot: Enter above, Done below.
	float tallHeight = key_unit * 2.f + key_gap;
	bool shiftMatters = overlay_keyboard_page != OverlayKeyboardPage::SYMBOLS;
	if (sideKey("##xlln_key_left", grid_origin.x, grid_origin.y, key_unit, "\xE2\x86\x90", "LB", false)) {
		action = OverlayKeyboardKeyAction::CARET_LEFT;
	}
	if (sideKey("##xlln_key_page", grid_origin.x, grid_origin.y + step, tallHeight, nextPageLabel, "LT", false)) {
		action = OverlayKeyboardKeyAction::PAGE;
	}
	if (sideKey("##xlln_key_shift", grid_origin.x, grid_origin.y + 3.f * step, tallHeight, "Shift", "RT", shiftMatters && overlay_keyboard_shift)) {
		action = OverlayKeyboardKeyAction::SHIFT;
	}
	if (sideKey("##xlln_key_right", rightX, grid_origin.y, key_unit, "\xE2\x86\x92", "RB", false)) {
		action = OverlayKeyboardKeyAction::CARET_RIGHT;
	}
	if (sideKey("##xlln_key_back", rightX, grid_origin.y + step, tallHeight, "Back", "X", false)) {
		action = OverlayKeyboardKeyAction::BACKSPACE;
	}
	float doneY = grid_origin.y + 3.f * step;
	float doneHeight = tallHeight;
	if (is_multiline) {
		if (sideKey("##xlln_key_enter", rightX, doneY, key_unit, "Enter", 0, false)) {
			action = OverlayKeyboardKeyAction::NEWLINE;
		}
		doneY += step;
		doneHeight = key_unit;
	}
	OverlayKeyboardFocusNext(&overlay_keyboard_focus_done);
	if (sideKey("##xlln_key_done", rightX, doneY, doneHeight, "Done", "Menu", false)) {
		action = OverlayKeyboardKeyAction::DONE;
	}
	float spaceY = grid_origin.y + 4.f * step;
	if (OverlayKeyboardKey("##xlln_key_space", ImVec2(middleX, spaceY), ImVec2((float)OVERLAY_KEYBOARD_COLUMNS * step - key_gap, key_unit), "Space", pad_connected ? "Y" : 0, false)) {
		action = OverlayKeyboardKeyAction::SPACE;
	}
	return action;
}

void OverlayKeyboardBuild()
{
	OverlayKeyboardTakeIncoming();
	if (!overlay_keyboard_active_valid) {
		return;
	}

	ImGuiIO &imguiIo = ImGui::GetIO();

	// A second dim layer. The guide already dims the game, this dims the guide too.
	ImGui::GetBackgroundDrawList()->AddRectFilled(
		ImVec2(0.f, 0.f)
		, imguiIo.DisplaySize
		, OverlayDimColor(0.55f)
	);

	const bool isPassword = (overlay_keyboard_active.flags & VKBD_LATIN_PASSWORD) != 0;
	// The text widget cannot mask a multi-line field. Hiding the characters matters more than
	// accepting more than one line, so a request for both is answered with one masked line.
	const bool isMultiline = !isPassword && (overlay_keyboard_active.flags & VKBD_MULTILINE) != 0;
	const bool isNumeric = (overlay_keyboard_active.flags & OVERLAY_KEYBOARD_SETS_NUMERIC) != 0;
	const bool padConnected = OverlayGamepadIsConnected();

	float bodyPadding = OverlayPx(18.f);
	float keyUnit = OverlayPx(38.f);
	float keyGap = OverlayPx(4.f);
	float gridWidth = isNumeric
		? keyUnit * 4.5f + keyGap * 3.f
		: keyUnit * 13.f + keyGap * 11.f;
	float gridHeight = isNumeric
		? keyUnit * 4.f + keyGap * 3.f
		: keyUnit * 5.f + keyGap * 4.f;

	// Wide enough for the grid, and never narrower than the field alone was.
	float modalWidth = OverlayPx(500.f);
	if (gridWidth + bodyPadding * 2.f > modalWidth) {
		modalWidth = gridWidth + bodyPadding * 2.f;
	}
	if (modalWidth > imguiIo.DisplaySize.x - OverlayPx(32.f)) {
		modalWidth = imguiIo.DisplaySize.x - OverlayPx(32.f);
	}
	float contentWidth = modalWidth - bodyPadding * 2.f;

	ImGui::SetNextWindowPos(ImVec2(imguiIo.DisplaySize.x * 0.5f, imguiIo.DisplaySize.y * 0.47f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(modalWidth, 0.f), ImGuiCond_Always);
	ImGui::SetNextWindowFocus();
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, OverlayPx(18.f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(bodyPadding, bodyPadding));
	ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(OVERLAY_COL_BORDER_HI));
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(OVERLAY_COL_GLASS_2));
	if (ImGui::Begin(
		"##xlln_keyboard_modal"
		, 0
		, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
		| ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse
	)) {
		ImVec2 windowPosition = ImGui::GetWindowPos();

		std::string titleUtf8 = OverlayKeyboardToUtf8(overlay_keyboard_active.titleText);
		ImGui::PushFont(overlay_fonts.bold17);
		ImGui::TextUnformatted(titleUtf8.empty() ? "Enter text" : titleUtf8.c_str());
		ImGui::PopFont();

		std::string descriptionUtf8 = OverlayKeyboardToUtf8(overlay_keyboard_active.descriptionText);
		if (!descriptionUtf8.empty()) {
			ImGui::Dummy(ImVec2(0.f, OverlayPx(5.f)));
			ImGui::PushFont(overlay_fonts.reg12);
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(OVERLAY_COL_TEXT_DIM));
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + contentWidth);
			ImGui::TextUnformatted(descriptionUtf8.c_str());
			ImGui::PopTextWrapPos();
			ImGui::PopStyleColor();
			ImGui::PopFont();
		}

		ImGui::Dummy(ImVec2(0.f, OverlayPx(10.f)));

		// VKBD_HIGHLIGHT_TEXT asks for the text that is already there to come up selected, so the
		// first thing the player types replaces it.
		ImGuiInputTextFlags inputFlags = ImGuiInputTextFlags_CallbackAlways;
		if (overlay_keyboard_active.flags & VKBD_HIGHLIGHT_TEXT) {
			inputFlags |= ImGuiInputTextFlags_AutoSelectAll;
		}
		if (isPassword) {
			inputFlags |= ImGuiInputTextFlags_Password;
		}
		if (!isMultiline) {
			inputFlags |= ImGuiInputTextFlags_EnterReturnsTrue;
		}

		if (overlay_keyboard_take_focus) {
			overlay_keyboard_take_focus = false;
			ImGui::SetKeyboardFocusHere();
		}

		ImVec2 framePadding(OverlayPx(11.f), OverlayPx(10.f));
		ImGui::PushFont(overlay_fonts.reg13);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, framePadding);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, OverlayPx(7.5f));
		// On the light theme the field is nearly the window colour, so the border is what shows it.
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
		ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::ColorConvertU32ToFloat4(overlay_palette.fieldBg));
		ImGui::SetNextItemWidth(contentWidth);
		bool committed = false;
		if (isMultiline) {
			ImGui::InputTextMultiline(
				"##xlln_keyboard_field"
				, overlay_keyboard_buffer.data()
				, overlay_keyboard_buffer.size()
				, ImVec2(contentWidth, OverlayPx(78.f))
				, inputFlags
				, OverlayKeyboardFieldCallback
			);
		}
		else {
			committed = ImGui::InputText(
				"##xlln_keyboard_field"
				, overlay_keyboard_buffer.data()
				, overlay_keyboard_buffer.size()
				, inputFlags
				, OverlayKeyboardFieldCallback
			);
		}
		overlay_keyboard_field_active = ImGui::IsItemActive();
		overlay_keyboard_text_focus.store(overlay_keyboard_field_active);
		if (!overlay_keyboard_field_active) {
			OverlayKeyboardDrawCaret(overlay_fonts.reg13, framePadding, isPassword, isMultiline);
		}
		ImGui::PopStyleColor();
		ImGui::PopStyleVar(3);
		ImGui::PopFont();

		// The Title's buffer is the only limit there is, and it stops accepting text without saying
		// so. The count is of characters, while the buffer limits UTF-8 bytes, so a line of accented
		// text can stop one or two short of the number shown.
		if (overlay_keyboard_active.resultTextSize > 1) {
			size_t usedCharacters = 0;
			for (const char* scan = overlay_keyboard_buffer.data(); *scan; scan++) {
				if (OverlayKeyboardIsCodePointStart(*scan)) {
					usedCharacters++;
				}
			}
			char counterText[48];
			snprintf(counterText, sizeof(counterText), "%zu / %zu", usedCharacters, overlay_keyboard_active.resultTextSize - 1);
			ImGui::Dummy(ImVec2(0.f, OverlayPx(4.f)));
			ImGui::PushFont(overlay_fonts.reg12);
			float counterWidth = ImGui::CalcTextSize(counterText).x;
			if (counterWidth < contentWidth) {
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + contentWidth - counterWidth);
			}
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(OVERLAY_COL_TEXT_FAINT));
			ImGui::TextUnformatted(counterText);
			ImGui::PopStyleColor();
			ImGui::PopFont();
		}

		ImGui::Dummy(ImVec2(0.f, OverlayPx(8.f)));

		// Explicit positions, because two keys span two rows, which the layout cursor cannot express.
		ImVec2 gridOrigin = ImGui::GetCursorScreenPos();
		gridOrigin.x += (contentWidth - gridWidth) * 0.5f;
		const char* insertText = 0;
		OverlayKeyboardKeyAction keyAction = OverlayKeyboardBuildGrid(gridOrigin, keyUnit, keyGap, isNumeric, isMultiline, padConnected, &insertText);
		ImGui::SetCursorScreenPos(ImVec2(gridOrigin.x, gridOrigin.y + gridHeight));
		ImGui::Dummy(ImVec2(0.f, OverlayPx(10.f)));

		// Pad bindings apply only while the field is inactive; an active field uses them for its caret.
		bool cancelled = false;
		if (padConnected && !overlay_keyboard_field_active) {
			if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false)) {
				keyAction = OverlayKeyboardKeyAction::SPACE;
			}
			if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, true)) {
				keyAction = OverlayKeyboardKeyAction::CARET_LEFT;
			}
			if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, true)) {
				keyAction = OverlayKeyboardKeyAction::CARET_RIGHT;
			}
			if (ImGui::IsKeyPressed(ImGuiKey_GamepadL2, false)) {
				keyAction = OverlayKeyboardKeyAction::PAGE;
			}
			if (ImGui::IsKeyPressed(ImGuiKey_GamepadR2, false)) {
				keyAction = OverlayKeyboardKeyAction::SHIFT;
			}
			if (ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false)) {
				keyAction = OverlayKeyboardKeyAction::DONE;
			}
			if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) && !ImGui::IsAnyItemActive()) {
				cancelled = true;
			}
			// X repeats while it is held, at the rate a held key repeats.
			double timeNow = ImGui::GetTime();
			if (OverlayGamepadFaceLeftDown()) {
				if (overlay_keyboard_backspace_next_repeat < 0.0) {
					keyAction = OverlayKeyboardKeyAction::BACKSPACE;
					overlay_keyboard_backspace_next_repeat = timeNow + imguiIo.KeyRepeatDelay;
				}
				else if (timeNow >= overlay_keyboard_backspace_next_repeat) {
					keyAction = OverlayKeyboardKeyAction::BACKSPACE;
					overlay_keyboard_backspace_next_repeat = timeNow + imguiIo.KeyRepeatRate;
				}
			}
			else {
				overlay_keyboard_backspace_next_repeat = -1.0;
			}
		}

		// A mouse press on a key takes focus off the field first, so the field is never active here.
		switch (keyAction) {
			case OverlayKeyboardKeyAction::INSERT: {
				if (insertText) {
					OverlayKeyboardInsert(insertText);
				}
				break;
			}
			case OverlayKeyboardKeyAction::SPACE: {
				OverlayKeyboardInsert(" ");
				break;
			}
			case OverlayKeyboardKeyAction::NEWLINE: {
				OverlayKeyboardInsert("\n");
				break;
			}
			case OverlayKeyboardKeyAction::BACKSPACE: {
				OverlayKeyboardBackspace();
				break;
			}
			case OverlayKeyboardKeyAction::CARET_LEFT: {
				OverlayKeyboardMoveCaret(-1);
				break;
			}
			case OverlayKeyboardKeyAction::CARET_RIGHT: {
				OverlayKeyboardMoveCaret(1);
				break;
			}
			case OverlayKeyboardKeyAction::SHIFT: {
				overlay_keyboard_shift = !overlay_keyboard_shift;
				break;
			}
			case OverlayKeyboardKeyAction::PAGE: {
				OverlayKeyboardNextPage();
				break;
			}
			case OverlayKeyboardKeyAction::DONE: {
				committed = true;
				break;
			}
			default: {
				break;
			}
		}

		// The closing keys, named once under the grid; Done is a grid key, so no button repeats it.
		{
			struct _OVERLAY_KEYBOARD_HINT {
				const char* keycap;
				const char* action;
			} hints[2];
			int hintCount = 0;
			if (padConnected) {
				hints[hintCount++] = { "B", "Cancel" };
			}
			else {
				hints[hintCount++] = { "Esc", "Cancel" };
				if (!isMultiline) {
					hints[hintCount++] = { "Enter", "Done" };
				}
			}
			ImFont* hintFont = overlay_fonts.reg12;
			float keycapHeight = OverlayKeycapHeight(false);
			float keycapToText = OverlayPx(6.f);
			float betweenHints = OverlayPx(16.f);
			float hintsWidth = 0.f;
			for (int iHint = 0; iHint < hintCount; iHint++) {
				hintsWidth += (iHint ? betweenHints : 0.f)
					+ OverlayKeycapWidth(hints[iHint].keycap, false)
					+ keycapToText
					+ hintFont->CalcTextSizeA(hintFont->FontSize, FLT_MAX, 0.f, hints[iHint].action).x;
			}
			if (hintsWidth < contentWidth) {
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + contentWidth - hintsWidth);
			}
			float rowTop = ImGui::GetCursorPosY();
			for (int iHint = 0; iHint < hintCount; iHint++) {
				if (iHint) {
					ImGui::SameLine(0.f, betweenHints);
					ImGui::SetCursorPosY(rowTop);
				}
				OverlayKeycapItem(hints[iHint].keycap, false);
				ImGui::SameLine(0.f, keycapToText);
				ImGui::SetCursorPosY(rowTop + (keycapHeight - hintFont->FontSize) * 0.5f);
				ImGui::PushFont(hintFont);
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(OVERLAY_COL_TEXT_FAINT));
				ImGui::TextUnformatted(hints[iHint].action);
				ImGui::PopStyleColor();
				ImGui::PopFont();
			}
		}

		ImVec2 windowSize = ImGui::GetWindowSize();
		OverlayDrawSoftShadow(
			ImGui::GetBackgroundDrawList()
			, windowPosition
			, ImVec2(windowPosition.x + windowSize.x, windowPosition.y + windowSize.y)
			, OverlayPx(18.f)
			, OverlayPx(12.f)
			, OverlayPx(26.f)
			, 20
		);

		// Escape is read here rather than by the guide, so backing out of the text answers the
		// Title instead of closing the guide under it.
		if (!cancelled && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
			cancelled = true;
		}

		if (committed) {
			OverlayKeyboardFinish(ERROR_SUCCESS);
		}
		else if (cancelled) {
			OverlayKeyboardFinish(ERROR_CANCELLED);
		}
	}
	ImGui::End();
	ImGui::PopStyleColor(2);
	ImGui::PopStyleVar(2);
}
