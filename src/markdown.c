#include "markdown.h"

#include <stdlib.h>
#include <string.h>

#include "highlight.h"
#include "layout.h"
#include "memory.h"
#include "move.h"
#include "utf8.h"

// the narrowest a column gets, room for the delimiter ":-:".
static constexpr u64 minColumnWidth = 3;

// a piece of a line: a cell's text, or what is left of it to wrap.
typedef struct Span {
  const char *text;
  u64 length;
} Span;

// the cells of a row, as a run of the table's cells.
typedef struct Row {
  u64 first;
  u32 count;
} Row;

// a column's width, and the alignment colons of its delimiter cell.
typedef struct Column {
  u64 width;
  bool left;
  bool right;
} Column;

// a table as read from its lines: the text before its header (indentation and quote
// markers), the cells of every row (the second row being the delimiter row) and the
// columns, as many as the most cells in a row.
typedef struct Table {
  Span prefix;

  Span *cells;
  u64 cellCount;
  u64 cellCapacity;

  Row *rows;
  u64 rowCount;

  Column *columns;
  u32 columnCount;
} Table;

static bool
IsBlank(char character)
{
  return character == ' ' || character == '\t';
}

static Span
Trim(Span span)
{
  while (span.length && IsBlank(span.text[0])) {
    span.text++;
    span.length--;
  }

  while (span.length && IsBlank(span.text[span.length - 1])) span.length--;
  return span;
}

static u64
Width(Span span)
{
  return LayoutColumn(span.text, span.length, span.length);
}

static void
AppendRepeated(StrBuf *text, char character, u64 count)
{
  for (u64 index = 0; index < count; index++) StrBufAppendByte(text, character);
}

// the lines of the table that holds position, first to last.
static bool
TableLines(Document *document, Position position, u64 *first, u64 *last)
{
  Range range;
  if (!HighlightNodeOfType(document->highlight, position, "pipe_table", &range)) return false;

  // the line break after the last row ends the table at the start of the next line.
  bool atLineStart = range.end.byte == 0 && range.end.line > range.start.line;
  *first = range.start.line;
  *last = atLineStart ? range.end.line - 1 : range.end.line;
  return true;
}

// the length of the indentation and quote markers before a row.
static u64
PrefixLength(const StrBuf *line)
{
  u64 length = 0;

  while (length < line->length) {
    char character = line->data[length];
    if (!IsBlank(character) && character != '>') break;
    length++;
  }

  return length;
}

static void
AddCell(Table *table, Row *row, Span text)
{
  if (table->cellCount == table->cellCapacity) {
    table->cellCapacity = table->cellCapacity ? table->cellCapacity * 2 : 64;
    table->cells = MemoryResize(table->cells, table->cellCapacity, sizeof(Span));
  }

  table->cells[table->cellCount++] = Trim(text);
  row->count++;
}

// splits a row (after its prefix) into cells at the pipes that are not escaped, leaving out
// the pipes that open and close it.
static void
ReadRow(Table *table, Span text, Row *row)
{
  text = Trim(text);
  row->first = table->cellCount;
  row->count = 0;

  u64 start = 0;
  if (text.length && text.text[0] == '|') start = 1;

  for (u64 at = start; at < text.length; at++) {
    if (text.text[at] == '\\') {
      at++;
      continue;
    }

    if (text.text[at] != '|') continue;

    AddCell(table, row, (Span){text.text + start, at - start});
    start = at + 1;
  }

  // the text after the last pipe is a cell, unless the pipe closed the row.
  if (start < text.length || !row->count) {
    AddCell(table, row, (Span){text.text + start, text.length - start});
  }
}

static void
ReadTable(const Buffer *buffer, u64 first, u64 last, Table *table)
{
  const StrBuf *header = BufferLine(buffer, first);
  table->prefix = (Span){header->data, PrefixLength(header)};
  table->rowCount = last - first + 1;
  table->rows = MemoryAllocate(table->rowCount * sizeof(Row));

  for (u64 row = 0; row < table->rowCount; row++) {
    const StrBuf *line = BufferLine(buffer, first + row);
    u64 prefix = PrefixLength(line);
    ReadRow(table, (Span){line->data + prefix, line->length - prefix}, &table->rows[row]);

    u32 count = table->rows[row].count;
    if (count > table->columnCount) table->columnCount = count;
  }

  table->columns = MemoryAllocateZeroed(table->columnCount, sizeof(Column));
  if (table->rowCount < 2) return;

  const Row *delimiter = &table->rows[1];

  for (u32 column = 0; column < delimiter->count; column++) {
    Span cell = table->cells[delimiter->first + column];
    table->columns[column].left = cell.length && cell.text[0] == ':';
    table->columns[column].right = cell.length && cell.text[cell.length - 1] == ':';
  }
}

static void
FreeTable(Table *table)
{
  free(table->cells);
  free(table->rows);
  free(table->columns);
}

// the text of a cell, empty for a cell its row lacks.
static Span
CellAt(const Table *table, u64 row, u32 column)
{
  const Row *cells = &table->rows[row];
  if (column >= cells->count) return (Span){"", 0};
  return table->cells[cells->first + column];
}

// takes the next line of a wrapped cell from the front of rest, at most width columns
// wide, and returns false when nothing is left.
// a line breaks at the last blank that leaves its text fitting, dropping the blanks there;
// a word too long for a line of its own breaks between characters, never between a
// backslash and the character it escapes (so "\|" stays one).
static bool
NextPiece(Span *rest, u64 width, Span *piece)
{
  *piece = (Span){rest->text, 0};
  if (!rest->length) return false;

  LayoutIterator iterator = LayoutBegin(rest->text, rest->length);
  LayoutCluster cluster;

  // the end of the text that fits, and of its last word followed by a blank.
  u64 fits = 0;
  u64 wordEnd = 0;
  bool inWord = false;
  bool escaped = false;
  bool overflow = false;

  while (LayoutNext(&iterator, &cluster)) {
    bool blank = cluster.kind == LayoutKindSpace || cluster.kind == LayoutKindTab;

    if (blank && !escaped) {
      if (inWord) wordEnd = cluster.start;
      inWord = false;
      continue;
    }

    if (cluster.column + cluster.width > width) {
      overflow = true;
      break;
    }

    escaped = !escaped && rest->text[cluster.start] == '\\';
    inWord = true;
    if (!escaped) fits = cluster.end;
  }

  u64 end = rest->length;

  if (overflow) end = wordEnd ? wordEnd : fits;

  // a character (or escape) wider than the line still takes one.
  if (!end) {
    end = Utf8NextBoundary(rest->text, rest->length, 0);
    if (rest->text[0] == '\\') end = Utf8NextBoundary(rest->text, rest->length, end);
  }

  *piece = Trim((Span){rest->text, end});
  *rest = Trim((Span){rest->text + end, rest->length - end});
  return true;
}

// sets each column's width: as wide as its header cell, which never wraps, and its widest
// line of a body cell wrapped to maxWidth (or to the header, when that is wider), but no
// narrower than minColumnWidth.
// wrapping a cell again to its column's width then breaks it in the same places.
static void
MeasureColumns(Table *table, u64 maxWidth)
{
  for (u32 column = 0; column < table->columnCount; column++) {
    table->columns[column].width = Width(CellAt(table, 0, column));
  }

  for (u64 row = 2; row < table->rowCount; row++) {
    for (u32 column = 0; column < table->columnCount; column++) {
      Column *measured = &table->columns[column];
      u64 wrap = measured->width > maxWidth ? measured->width : maxWidth;
      Span rest = CellAt(table, row, column);
      Span piece;

      while (NextPiece(&rest, wrap, &piece)) {
        u64 width = Width(piece);
        if (width > measured->width) measured->width = width;
      }
    }
  }

  for (u32 column = 0; column < table->columnCount; column++) {
    Column *measured = &table->columns[column];
    if (measured->width < minColumnWidth) measured->width = minColumnWidth;
  }
}

static void
StartLine(const Table *table, StrBuf *text)
{
  if (text->length) StrBufAppendByte(text, '\n');
  StrBufAppend(text, table->prefix.text, table->prefix.length);
  StrBufAppendByte(text, '|');
}

static void
AppendDelimiter(const Table *table, StrBuf *text)
{
  StartLine(table, text);

  for (u32 column = 0; column < table->columnCount; column++) {
    const Column *aligned = &table->columns[column];
    StrBufAppendByte(text, ' ');
    if (aligned->left) StrBufAppendByte(text, ':');
    AppendRepeated(text, '-', aligned->width - (u64)aligned->left - (u64)aligned->right);
    if (aligned->right) StrBufAppendByte(text, ':');
    StrBufAppendString(text, " |");
  }
}

// appends the lines of a row, each cell wrapped to its column's width, and returns how
// many there are.
// rests has room for a span per column.
static u64
AppendRow(const Table *table, u64 row, Span *rests, StrBuf *text)
{
  u64 lines = 0;
  bool more;

  for (u32 column = 0; column < table->columnCount; column++) {
    rests[column] = CellAt(table, row, column);
  }

  do {
    StartLine(table, text);
    more = false;

    for (u32 column = 0; column < table->columnCount; column++) {
      u64 width = table->columns[column].width;
      Span piece;
      NextPiece(&rests[column], width, &piece);
      StrBufAppendByte(text, ' ');
      StrBufAppend(text, piece.text, piece.length);
      AppendRepeated(text, ' ', width - Width(piece));
      StrBufAppendString(text, " |");
      more = more || rests[column].length;
    }

    lines++;
  } while (more);

  return lines;
}

// the cell of its row that byte is in, by the pipes before it.
static u32
CellOf(const StrBuf *line, u64 byte)
{
  u64 prefix = PrefixLength(line);
  bool leadingPipe = prefix < line->length && line->data[prefix] == '|';
  u64 end = byte < line->length ? byte : line->length;
  u32 pipes = 0;

  for (u64 at = prefix; at < end; at++) {
    if (line->data[at] == '\\') at++;
    else if (line->data[at] == '|') pipes++;
  }

  return leadingPipe && pipes ? pipes - 1 : pipes;
}

// the byte where a column's text starts on an aligned line.
static u64
ColumnStart(const Table *table, u32 column)
{
  // the prefix and "|", then " text |" for each column before it, and its own " ".
  u64 start = table->prefix.length + 1;
  for (u32 index = 0; index < column; index++) start += table->columns[index].width + 3;
  return start + 1;
}

bool
MarkdownInTable(Document *document)
{
  Position head = CursorSetPrimary(&document->view->cursors)->head;
  u64 first;
  u64 last;
  return TableLines(document, head, &first, &last);
}

bool
MarkdownAlignTable(Document *document, u64 maxWidth, MarkdownTableResult *result)
{
  View *view = document->view;
  Buffer *buffer = &document->buffer;
  Position head = CursorSetPrimary(&view->cursors)->head;
  u64 first;
  u64 last;
  if (!TableLines(document, head, &first, &last)) return false;

  Table table = {};
  ReadTable(buffer, first, last, &table);
  MeasureColumns(&table, maxWidth);

  u64 cursorRow = head.line - first;
  u32 cursorColumn = CellOf(BufferLine(buffer, head.line), head.byte);
  if (cursorColumn >= table.columnCount) cursorColumn = table.columnCount - 1;

  StrBuf text = {};
  Span *rests = MemoryAllocate(table.columnCount * sizeof(Span));
  u64 lines = 0;
  Position cursor = {first, ColumnStart(&table, cursorColumn)};

  for (u64 row = 0; row < table.rowCount; row++) {
    if (row == cursorRow) cursor.line = first + lines;

    if (row == 1) {
      AppendDelimiter(&table, &text);
      lines++;
    } else {
      lines += AppendRow(&table, row, rests, &text);
    }
  }

  Range range = {{first, 0}, {last, BufferLine(buffer, last)->length}};
  StrBuf old = {};
  BufferGetText(buffer, range, &old);
  bool changed = old.length != text.length || memcmp(old.data, text.data, text.length) != 0;

  UndoBegin(&document->undo, UndoKindOther, &view->cursors);

  // an aligned table is left alone, which keeps the history clean.
  if (changed) BufferReplace(buffer, range, text.data, text.length);

  MoveSelect(view, cursor, cursor);
  UndoEnd(&document->undo, &view->cursors);

  *result = (MarkdownTableResult){table.columnCount, lines, changed};
  StrBufFree(&old);
  StrBufFree(&text);
  free(rests);
  FreeTable(&table);
  return true;
}
