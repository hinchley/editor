#pragma once

// a grid of styled cells that is drawn in full each frame and then sent to the terminal.
// only cells that differ from the previous frame are written, inside one synchronized
// update, so the terminal never shows a half drawn frame.
#include "types.h"

// colour value meaning the terminal's default colour.
static constexpr i16 colorDefault = -1;

enum StyleAttribute : u8 {
  StyleAttributeBold = 1,
  StyleAttributeFaint = 2,
  StyleAttributeItalic = 4,
  StyleAttributeUnderline = 8,
  StyleAttributeReverse = 16
};

// foreground and background are 256 colour palette indexes or colorDefault.
typedef struct Style {
  i16 foreground;
  i16 background;
  u8 attributes;
} Style;

static constexpr Style styleDefault = {colorDefault, colorDefault, 0};

// resizes the grid and forces the next flush to redraw everything.
void
ScreenResize(i32 width, i32 height);

// starts a frame: every cell becomes a blank default cell and the cursor is hidden.
void
ScreenBegin(void);

// draws text from row and column, clipped at the right edge.
// returns the column after the last cell drawn.
// control characters are shown as the replacement character, and zero width code points
// join the cell before them.
i32
ScreenPutText(i32 row, i32 column, const char *text, u64 length, Style style);

// same as ScreenPutText for a nul terminated string.
i32
ScreenPutString(i32 row, i32 column, const char *string, Style style);

// fills count cells from row and column with spaces in the given style.
void
ScreenFill(i32 row, i32 column, i32 count, Style style);

// shows the terminal cursor at row and column for this frame.
void
ScreenSetCursor(i32 row, i32 column);

// sends the frame to the terminal.
void
ScreenFlush(void);
