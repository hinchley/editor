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

  // a blank cell an image is drawn over, which is only written to clear what was there.
  bool image;
} Cell;

typedef struct Grid {
  Cell *cells;
  StrBuf text;
} Grid;

// an image over a rectangle of cells, and the key of the picture it is.
typedef struct Placement {
  i32 row;
  i32 column;
  i32 columns;
  i32 rows;
  u64 key;

  // the sixel sequence that draws it, valid until the frame is flushed.
  const StrBuf *sixel;
} Placement;

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

// the images placed in the frame being drawn, and those the terminal shows.
static Placement placed[screenMaxImages];
static u32 placedCount;
static Placement shownImages[screenMaxImages];
static u32 shownImageCount;

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
    a.attributes == b.attributes && a.underline == b.underline;
}

static bool
CellEqual(const Cell *a, const Cell *b)
{
  if (a->width != b->width || a->textLength != b->textLength || a->image != b->image) {
    return false;
  }

  if (!StyleEqual(a->style, b->style)) return false;
  return memcmp(CellText(&back, a), CellText(&front, b), a->textLength) == 0;
}

static void
SetBlank(i32 row, i32 column, Style style)
{
  // offset 0 of every frame's text is a single space.
  *CellAt(&back, row, column) = (Cell){0, 1, 1, style, false};
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
  *cell = (Cell){(u32)back.text.length, (u8)length, (u8)width, style, false};
  StrBufAppend(&back.text, text, length);

  if (width == 2) *CellAt(&back, row, column + 1) = (Cell){0, 0, 0, style, false};
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
  placedCount = 0;
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
      size = sizeof(replacementText) - 1;
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
ScreenPutImage(i32 row, i32 column, i32 columns, i32 rows, const StrBuf *sixel, u64 key)
{
  // the terminal would scroll for an image that reached the bottom row.
  bool fits = row >= 0 && column >= 0 && row + rows < screenHeight &&
    column + columns <= screenWidth;
  if (!fits || columns <= 0 || rows <= 0 || placedCount == screenMaxImages) return;

  placed[placedCount++] = (Placement){row, column, columns, rows, key, sixel};

  for (i32 y = row; y < row + rows; y++) {
    for (i32 x = column; x < column + columns; x++) {
      BreakWide(y, x);
      SetBlank(y, x, styleDefault);
      CellAt(&back, y, x)->image = true;
    }
  }
}

void
ScreenSetCursor(i32 row, i32 column)
{
  cursorRow = row;
  cursorColumn = column;
  cursorVisible = row >= 0 && row < screenHeight && column >= 0 && column < screenWidth;
}

// writes the escape sequence that moves the cursor to row and column.
static void
WriteMove(i32 row, i32 column)
{
  StrBufAppendString(&output, "\x1b[");
  StrBufAppendNumber(&output, (u64)row + 1);
  StrBufAppendByte(&output, ';');
  StrBufAppendNumber(&output, (u64)column + 1);
  StrBufAppendByte(&output, 'H');
}

// writes one colour of a style: the parameters that introduce it, then its number.
static void
WriteColour(const char *introducer, i16 colour)
{
  StrBufAppendString(&output, introducer);
  StrBufAppendNumber(&output, (u64)colour);
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
  if (style.attributes & StyleAttributeCurly) StrBufAppendString(&output, ";4:3");

  if ((style.attributes & StyleAttributeCurly) && style.underline != colorDefault) {
    WriteColour(";58:5:", style.underline);
  }

  if (style.foreground != colorDefault) WriteColour(";38;5;", style.foreground);
  if (style.background != colorDefault) WriteColour(";48;5;", style.background);

  StrBufAppendByte(&output, 'm');

  writer->style = style;
  writer->styleKnown = true;
}

// an image cell is never plain, since writing it again would clear the image over it.
static bool
IsPlainCell(const Cell *cell)
{
  return cell->width == 1 && cell->textLength == 1 && !cell->image &&
    (u8)CellText(&back, cell)[0] < 0x80;
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
    WriteMove(row, column);
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

// true when a placement covers a whole rectangle of image cells, with nothing drawn over
// any of them after it was placed.
static bool
Uncovered(const Placement *placement)
{
  i32 bottom = placement->row + placement->rows;
  i32 right = placement->column + placement->columns;

  for (i32 row = placement->row; row < bottom; row++) {
    for (i32 column = placement->column; column < right; column++) {
      if (!CellAt(&back, row, column)->image) return false;
    }
  }

  return true;
}

// turns the image cells of a placement back into the plain blanks they hold.
static void
Unmark(const Placement *placement)
{
  i32 bottom = placement->row + placement->rows;
  i32 right = placement->column + placement->columns;

  for (i32 row = placement->row; row < bottom; row++) {
    for (i32 column = placement->column; column < right; column++) {
      CellAt(&back, row, column)->image = false;
    }
  }
}

// leaves out the images something was drawn over, making the rest of their cells plain
// blanks, so they are drawn again once nothing covers them.
static void
DropCovered(void)
{
  u32 kept = 0;

  for (u32 index = 0; index < placedCount; index++) {
    Placement *placement = &placed[index];

    if (Uncovered(placement)) placed[kept++] = *placement;
    else Unmark(placement);
  }

  placedCount = kept;
}

static bool
PlacementEqual(const Placement *a, const Placement *b)
{
  return a->row == b->row && a->column == b->column && a->columns == b->columns &&
    a->rows == b->rows && a->key == b->key;
}

// true when the frame places other images than the terminal shows, or the same ones
// elsewhere.
static bool
ImagesChanged(void)
{
  if (placedCount != shownImageCount) return true;

  for (u32 index = 0; index < placedCount; index++) {
    bool found = false;

    for (u32 other = 0; other < shownImageCount && !found; other++) {
      found = PlacementEqual(&placed[index], &shownImages[other]);
    }

    if (!found) return true;
  }

  return false;
}

// writes the cells that changed, and when the images changed every image cell, which clears
// whatever was under the new ones.
static bool
WriteChangedCells(bool imagesChanged)
{
  Writer writer = {.row = -1};
  bool changed = false;

  for (i32 row = 0; row < screenHeight; row++) {
    for (i32 column = 0; column < screenWidth; column++) {
      // the right half of a wide character is drawn with its left half.
      const Cell *cell = CellAt(&back, row, column);
      if (cell->width == 0) continue;

      bool forced = redrawAll || (cell->image && imagesChanged);
      if (!forced && CellEqual(cell, CellAt(&front, row, column))) continue;

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

  DropCovered();

  // a full redraw clears the images too.
  bool imagesChanged = redrawAll || ImagesChanged();
  bool changed = WriteChangedCells(imagesChanged) || redrawAll;

  // the images go over the cells cleared for them.
  for (u32 index = 0; imagesChanged && index < placedCount; index++) {
    const Placement *placement = &placed[index];
    WriteMove(placement->row, placement->column);
    StrBufAppend(&output, placement->sixel->data, placement->sixel->length);
    changed = true;
  }

  for (u32 index = 0; index < placedCount; index++) {
    shownImages[index] = placed[index];
    shownImages[index].sixel = nullptr;
  }

  shownImageCount = placedCount;
  bool cursorMoved = cursorVisible != shownCursorVisible || cursorRow != shownCursorRow ||
    cursorColumn != shownCursorColumn;

  // swap so the frame just drawn becomes the one the terminal shows.
  Grid shown = back;
  back = front;
  front = shown;
  redrawAll = false;
  if (!changed && !cursorMoved) return;

  if (cursorVisible) {
    WriteMove(cursorRow, cursorColumn);
    StrBufAppendString(&output, "\x1b[?25h");
  }

  StrBufAppendString(&output, "\x1b[?2026l");
  TermWrite(output.data, output.length);

  shownCursorRow = cursorRow;
  shownCursorColumn = cursorColumn;
  shownCursorVisible = cursorVisible;
}
