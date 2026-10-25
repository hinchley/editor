#pragma once

// a grid of styled cells that is drawn in full each frame and then sent to the terminal.
// only cells that differ from the previous frame are written, inside one synchronized
// update, so the terminal never shows a half drawn frame.
#include "strbuf.h"
#include "types.h"

// colour value meaning the terminal's default colour.
static constexpr i16 colorDefault = -1;

enum StyleAttribute : u8 {
  StyleAttributeBold = 1,
  StyleAttributeFaint = 2,
  StyleAttributeItalic = 4,
  StyleAttributeUnderline = 8,
  StyleAttributeReverse = 16,

  // a curly underline in its own colour.
  StyleAttributeCurly = 32
};

// foreground and background are 256 colour palette indexes or colorDefault, and so is
// underline, the colour of a curly underline (only used with StyleAttributeCurly).
typedef struct Style {
  i16 foreground;
  i16 background;
  u8 attributes;
  i16 underline;
} Style;

static constexpr Style styleDefault = {colorDefault, colorDefault, 0, colorDefault};

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

// draws one grapheme cluster as a single cell of the given width (1 or 2), so a cluster
// whose width differs from its first code point (a flag, an emoji with a presentation
// selector) still lines up.
// the caller ensures the bytes contain no control characters.
// a cluster that does not fit before the right edge becomes a space.
void
ScreenPutCluster(i32 row, i32 column, const char *text, u64 length, u32 width, Style style);

// same as ScreenPutText for a nul terminated string.
i32
ScreenPutString(i32 row, i32 column, const char *string, Style style);

// fills count cells from row and column with spaces in the given style.
void
ScreenFill(i32 row, i32 column, i32 count, Style style);

// changes the style of the cell at row and column, both halves of a wide character.
void
ScreenSetStyle(i32 row, i32 column, Style style);

// the most images a frame can place, one for each side of a split.
static constexpr u32 screenMaxImages = 2;

// places an image over the rectangle of columns by rows cells from row and column for this
// frame: the cells are cleared, and the sixel sequence draws over them once flushed.
// key tells pictures apart, so an image is only sent again when it changes, moves, or
// something drawn over it has gone; sixel must stay valid until the frame is flushed.
// an image drawn over by anything placed after it is left out, and one that does not fit
// above the bottom row is not placed.
void
ScreenPutImage(i32 row, i32 column, i32 columns, i32 rows, const StrBuf *sixel, u64 key);

// shows the terminal cursor at row and column for this frame.
void
ScreenSetCursor(i32 row, i32 column);

// sends the frame to the terminal.
void
ScreenFlush(void);
