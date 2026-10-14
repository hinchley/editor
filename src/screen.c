#include "screen.h"

#include <string.h>

#include "memory.h"
#include "strbuf.h"
#include "term.h"
#include "utf8.h"

// one terminal cell.
// its text lives in the grid's text buffer so a cell can hold a whole grapheme cluster.
// a wide character takes two cells: the first has width 2 and the second width 0.
typedef struct Cell {
  u32 textOffset;
  u8 textLength;
  u8 width;
  Style style;
} Cell;

typedef struct Grid {
  Cell *cells;
  StrBuf text;
} Grid;

// where the frame writer last left the terminal cursor and pen.
typedef struct Writer {
  i32 row;
  i32 column;
  Style style;
  bool styleKnown;
} Writer;

// the frame being drawn and the frame the terminal currently shows.
static Grid back;
static Grid front;
static i32 screenWidth;
static i32 screenHeight;
static bool redrawAll = true;

static i32 cursorRow;
static i32 cursorColumn;
static bool cursorVisible;
static i32 shownCursorRow = -1;
static i32 shownCursorColumn = -1;
static bool shownCursorVisible;

static StrBuf output;

static constexpr char replacementText[] = "\xEF\xBF\xBD";

// unchanged cells between two changed ones are rewritten when there are at most this many.
static constexpr i32 maxFillGap = 6;

static Cell *
CellAt(Grid *grid, i32 row, i32 column)
{
  return &grid->cells[row * screenWidth + column];
}

static const char *
CellText(const Grid *grid, const Cell *cell)
{
  return grid->text.data + cell->textOffset;
}

static bool
StyleEqual(Style a, Style b)
{
  return a.foreground == b.foreground && a.background == b.background &&
    a.attributes == b.attributes;
}

static bool
CellEqual(const Cell *a, const Cell *b)
{
  if (a->width != b->width || a->textLength != b->textLength) return false;
  if (!StyleEqual(a->style, b->style)) return false;
  return memcmp(CellText(&back, a), CellText(&front, b), a->textLength) == 0;
}

static void
SetBlank(i32 row, i32 column, Style style)
{
  // offset 0 of every frame's text is a single space.
  *CellAt(&back, row, column) = (Cell){0, 1, 1, style};
}

static void
BreakWide(i32 row, i32 column)
{
  // overwriting either half of a wide character leaves the other half as a space.
  Cell *cell = CellAt(&back, row, column);

  if (cell->width == 0 && column > 0) SetBlank(row, column - 1, cell->style);
  if (cell->width == 2 && column + 1 < screenWidth) SetBlank(row, column + 1, cell->style);
}

static Cell *
SetCell(i32 row, i32 column, const char *text, u32 length, u32 width, Style style)
{
  BreakWide(row, column);
  if (width == 2) BreakWide(row, column + 1);

  Cell *cell = CellAt(&back, row, column);
  *cell = (Cell){(u32)back.text.length, (u8)length, (u8)width, style};
  StrBufAppend(&back.text, text, length);

  if (width == 2) *CellAt(&back, row, column + 1) = (Cell){0, 0, 0, style};
  return cell;
}

static void
JoinCell(Cell *cell, const char *text, u32 length)
{
  // only the most recent cell can grow, because its text ends the buffer.
  bool atEnd = cell->textOffset + cell->textLength == back.text.length;
  if (!atEnd || cell->textLength + length > 255) return;

  StrBufAppend(&back.text, text, length);
  cell->textLength = (u8)(cell->textLength + length);
}

void
ScreenResize(i32 width, i32 height)
{
  screenWidth = width > 0 ? width : 0;
  screenHeight = height > 0 ? height : 0;

  u64 count = (u64)screenWidth * (u64)screenHeight;
  back.cells = MemoryResize(back.cells, count, sizeof(Cell));
  front.cells = MemoryResize(front.cells, count, sizeof(Cell));
  redrawAll = true;
  ScreenBegin();
}

void
ScreenBegin(void)
{
  StrBufClear(&back.text);
  StrBufAppendByte(&back.text, ' ');

  for (i32 row = 0; row < screenHeight; row++) {
    for (i32 column = 0; column < screenWidth; column++) SetBlank(row, column, styleDefault);
  }

  cursorVisible = false;
}

i32
ScreenPutText(i32 row, i32 column, const char *text, u64 length, Style style)
{
  if (row < 0 || row >= screenHeight || column < 0) return column;

  Cell *previous = nullptr;
  u64 index = 0;

  while (index < length && column < screenWidth) {
    u32 codepoint;
    const char *bytes = text + index;
    u32 size = Utf8Decode(bytes, length - index, &codepoint);
    index += size;

    // control characters would act on the terminal instead of being shown.
    bool control = codepoint < 0x20 || (codepoint >= 0x7F && codepoint < 0xA0);

    if (control || codepoint == utf8Replacement) {
      codepoint = utf8Replacement;
      bytes = replacementText;
      size = sizeof replacementText - 1;
    }

    u32 width = Utf8Width(codepoint);

    if (width == 0) {
      if (previous) JoinCell(previous, bytes, size);
      continue;
    }

    // a wide character that does not fit is replaced by a space.
    if (column + (i32)width > screenWidth) {
      BreakWide(row, column);
      SetBlank(row, column, style);
      return screenWidth;
    }

    previous = SetCell(row, column, bytes, size, width, style);
    column += (i32)width;
  }

  return column;
}

void
ScreenPutCluster(i32 row, i32 column, const char *text, u64 length, u32 width, Style style)
{
  if (row < 0 || row >= screenHeight || column < 0 || column >= screenWidth) return;

  if (column + (i32)width > screenWidth) {
    BreakWide(row, column);
    SetBlank(row, column, style);
    return;
  }

  // a cell holds at most 255 bytes, so overlong clusters lose whole trailing code points.
  u64 size = 0;

  while (size < length) {
    u32 codepoint;
    u32 next = Utf8Decode(text + size, length - size, &codepoint);
    if (size + next > 255) break;
    size += next;
  }

  SetCell(row, column, text, (u32)size, width, style);
}

i32
ScreenPutString(i32 row, i32 column, const char *string, Style style)
{
  return ScreenPutText(row, column, string, strlen(string), style);
}

void
ScreenFill(i32 row, i32 column, i32 count, Style style)
{
  if (row < 0 || row >= screenHeight) return;

  i32 start = column > 0 ? column : 0;
  i32 end = column + count < screenWidth ? column + count : screenWidth;

  for (i32 index = start; index < end; index++) {
    BreakWide(row, index);
    SetBlank(row, index, style);
  }
}

void
ScreenSetStyle(i32 row, i32 column, Style style)
{
  if (row < 0 || row >= screenHeight || column < 0 || column >= screenWidth) return;

  Cell *cell = CellAt(&back, row, column);
  if (cell->width == 0 && column > 0) cell = CellAt(&back, row, --column);

  cell->style = style;
  bool wide = cell->width == 2 && column + 1 < screenWidth;
  if (wide) CellAt(&back, row, column + 1)->style = style;
}

void
ScreenSetCursor(i32 row, i32 column)
{
  cursorRow = row;
  cursorColumn = column;
  cursorVisible = row >= 0 && row < screenHeight && column >= 0 && column < screenWidth;
}

static void
WriteStyle(Writer *writer, Style style)
{
  if (writer->styleKnown && StyleEqual(writer->style, style)) return;

  StrBufAppendString(&output, "\x1b[0");
  if (style.attributes & StyleAttributeBold) StrBufAppendString(&output, ";1");
  if (style.attributes & StyleAttributeFaint) StrBufAppendString(&output, ";2");
  if (style.attributes & StyleAttributeItalic) StrBufAppendString(&output, ";3");
  if (style.attributes & StyleAttributeUnderline) StrBufAppendString(&output, ";4");
  if (style.attributes & StyleAttributeReverse) StrBufAppendString(&output, ";7");

  if (style.foreground != colorDefault) {
    StrBufAppendFormat(&output, ";38;5;%d", style.foreground);
  }

  if (style.background != colorDefault) {
    StrBufAppendFormat(&output, ";48;5;%d", style.background);
  }

  StrBufAppendByte(&output, 'm');

  writer->style = style;
  writer->styleKnown = true;
}

static bool
IsPlainCell(const Cell *cell)
{
  return cell->width == 1 && cell->textLength == 1 && (u8)CellText(&back, cell)[0] < 0x80;
}

static void
MoveTo(Writer *writer, i32 row, i32 column)
{
  // rewriting a short run of unchanged plain cells is cheaper than a cursor move.
  i32 gap = column - writer->column;
  bool fill = writer->row == row && gap > 0 && gap <= maxFillGap;

  for (i32 index = writer->column; fill && index < column; index++) {
    fill = IsPlainCell(CellAt(&back, row, index));
  }

  if (!fill) {
    StrBufAppendFormat(&output, "\x1b[%d;%dH", row + 1, column + 1);
    return;
  }

  for (i32 index = writer->column; index < column; index++) {
    const Cell *cell = CellAt(&back, row, index);
    WriteStyle(writer, cell->style);
    StrBufAppendByte(&output, CellText(&back, cell)[0]);
  }
}

static void
WriteCell(Writer *writer, i32 row, i32 column)
{
  const Cell *cell = CellAt(&back, row, column);

  if (writer->row != row || writer->column != column) MoveTo(writer, row, column);

  WriteStyle(writer, cell->style);
  const char *text = CellText(&back, cell);
  StrBufAppend(&output, text, cell->textLength);

  // the terminal may disagree about the width of non-ascii text, and writing the last
  // column leaves the cursor in a pending wrap state, so both force an explicit move.
  bool ascii = true;

  for (u32 index = 0; index < cell->textLength; index++) {
    if ((u8)text[index] >= 0x80) ascii = false;
  }

  writer->row = row;
  writer->column = column + cell->width;
  if (!ascii || writer->column >= screenWidth) writer->row = -1;
}

static bool
WriteChangedCells(void)
{
  Writer writer = {.row = -1};
  bool changed = false;

  for (i32 row = 0; row < screenHeight; row++) {
    for (i32 column = 0; column < screenWidth; column++) {
      // the right half of a wide character is drawn with its left half.
      const Cell *cell = CellAt(&back, row, column);
      if (cell->width == 0) continue;
      if (!redrawAll && CellEqual(cell, CellAt(&front, row, column))) continue;

      WriteCell(&writer, row, column);
      changed = true;
    }
  }

  return changed;
}

void
ScreenFlush(void)
{
  StrBufClear(&output);
  StrBufAppendString(&output, "\x1b[?2026h\x1b[?25l");
  if (redrawAll) StrBufAppendString(&output, "\x1b[0m\x1b[2J");

  bool changed = WriteChangedCells() || redrawAll;
  bool cursorMoved = cursorVisible != shownCursorVisible || cursorRow != shownCursorRow ||
    cursorColumn != shownCursorColumn;

  // swap so the frame just drawn becomes the one the terminal shows.
  Grid shown = back;
  back = front;
  front = shown;
  redrawAll = false;
  if (!changed && !cursorMoved) return;

  if (cursorVisible) {
    StrBufAppendFormat(&output, "\x1b[%d;%dH\x1b[?25h", cursorRow + 1, cursorColumn + 1);
  }

  StrBufAppendString(&output, "\x1b[?2026l");
  TermWrite(output.data, output.length);

  shownCursorRow = cursorRow;
  shownCursorColumn = cursorColumn;
  shownCursorVisible = cursorVisible;
}
