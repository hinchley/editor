#include "layout.h"

#include <stdlib.h>

#include "memory.h"
#include "settings.h"
#include "utf8.h"

static constexpr char ellipsis[] = "...";

LayoutIterator
LayoutBegin(const char *text, u64 length)
{
  return (LayoutIterator){text, length, 0, 0};
}

static void
ClassifyWide(const LayoutIterator *iterator, LayoutCluster *cluster)
{
  const char *bytes = iterator->text + cluster->start;
  u64 size = cluster->end - cluster->start;
  u32 codepoint;
  u32 first = Utf8Decode(bytes, size, &codepoint);

  // a decoded U+FFFD stands for an invalid byte unless the text really holds it.
  bool invalid = codepoint == utf8Replacement && first != 3;
  bool c1Control = codepoint >= 0x80 && codepoint < 0xA0;
  u32 width = Utf8ClusterWidth(bytes, size);

  if (invalid || c1Control || width == 0) {
    cluster->kind = LayoutKindInvalid;
    cluster->width = 1;
    return;
  }

  cluster->kind = LayoutKindText;
  cluster->width = width;
}

bool
LayoutNext(LayoutIterator *iterator, LayoutCluster *cluster)
{
  const char *text = iterator->text;
  u64 at = iterator->at;
  if (at >= iterator->length) return false;

  u8 byte = (u8)text[at];
  cluster->start = at;
  cluster->column = iterator->column;

  // printable ascii followed by ascii is always a cluster of its own.
  bool nextAscii = at + 1 >= iterator->length || (u8)text[at + 1] < 0x80;

  if (byte >= 0x20 && byte < 0x7F && nextAscii) {
    cluster->end = at + 1;
    cluster->width = 1;
    cluster->kind = byte == ' ' ? LayoutKindSpace : LayoutKindText;
  } else if (byte == '\t') {
    cluster->end = at + 1;
    cluster->width = (u32)(tabWidth - iterator->column % tabWidth);
    cluster->kind = LayoutKindTab;
  } else if (byte < 0x20 || byte == 0x7F) {
    // control characters are clusters of their own by the grapheme rules.
    cluster->end = at + 1;
    cluster->width = 2;
    cluster->kind = LayoutKindControl;
  } else {
    cluster->end = Utf8NextBoundary(text, iterator->length, at);
    ClassifyWide(iterator, cluster);
  }

  iterator->at = cluster->end;
  iterator->column += cluster->width;
  return true;
}

static bool
IsPrintableAscii(u8 byte)
{
  return byte >= 0x20 && byte < 0x7F;
}

// passes over the printable ascii before limit, one column per byte, without reading
// clusters.
// a byte followed by a non-ascii byte is left for LayoutNext, since a combining mark
// could join it.
static void
SkipAscii(LayoutIterator *iterator, u64 limit)
{
  const char *text = iterator->text;
  u64 start = iterator->at;
  u64 at = start;
  if (limit > iterator->length) limit = iterator->length;

  while (at < limit && IsPrintableAscii((u8)text[at])) at++;
  if (at > start && at < iterator->length && (u8)text[at] >= 0x80) at--;

  iterator->column += at - start;
  iterator->at = at;
}

// moves the iterator to the start of the first cluster that ends after byte, or that ends
// past column, whichever comes first.
static void
Seek(LayoutIterator *iterator, u64 byte, u64 column)
{
  LayoutCluster cluster;

  while (true) {
    // printable ascii is one column per byte, so the column gives a byte limit too.
    u64 columns = column - iterator->column;
    u64 bytes = byte > iterator->at ? byte - iterator->at : 0;
    SkipAscii(iterator, iterator->at + (columns < bytes ? columns : bytes));

    LayoutIterator before = *iterator;
    if (!LayoutNext(iterator, &cluster)) return;

    if (cluster.end > byte || cluster.column + cluster.width > column) {
      *iterator = before;
      return;
    }
  }
}

LayoutIterator
LayoutBeginAt(const char *text, u64 length, u64 byte)
{
  LayoutIterator iterator = LayoutBegin(text, length);
  Seek(&iterator, byte, UINT64_MAX);
  return iterator;
}

LayoutIterator
LayoutBeginAtColumn(const char *text, u64 length, u64 column)
{
  LayoutIterator iterator = LayoutBegin(text, length);
  Seek(&iterator, UINT64_MAX, column);
  return iterator;
}

u64
LayoutColumn(const char *text, u64 length, u64 byte)
{
  return LayoutBeginAt(text, length, byte).column;
}

static void
AddRow(LayoutRows *rows, u64 start)
{
  if (rows->count == rows->capacity) {
    rows->capacity = rows->capacity ? rows->capacity * 2 : 16;
    rows->starts = MemoryResize(rows->starts, rows->capacity, sizeof(u64));
  }

  rows->starts[rows->count++] = start;
}

void
LayoutWrap(const char *text, u64 length, i32 width, LayoutRows *rows)
{
  rows->count = 0;
  AddRow(rows, 0);
  if (width <= 0) return;

  // rows stop a column short, so the end of the line always has a column on its last row.
  u64 limit = width > 1 ? (u64)width - 1 : 1;
  LayoutIterator iterator = LayoutBegin(text, length);
  LayoutCluster cluster;
  u64 rowStart = 0;
  u64 rowWidth = 0;

  // the byte after the last space or tab in the row, and the row width up to it.
  u64 breakAt = 0;
  u64 breakWidth = 0;

  while (LayoutNext(&iterator, &cluster)) {
    while (rowWidth > 0 && rowWidth + cluster.width > limit) {
      if (breakAt > rowStart) {
        rowStart = breakAt;
        rowWidth -= breakWidth;
      } else {
        rowStart = cluster.start;
        rowWidth = 0;
      }

      AddRow(rows, rowStart);
      breakAt = 0;
    }

    rowWidth += cluster.width;

    if (cluster.kind == LayoutKindSpace || cluster.kind == LayoutKindTab) {
      breakAt = cluster.end;
      breakWidth = rowWidth;
    }
  }

  // only a cluster too wide for a row of its own can leave no column for the end.
  if (length > 0 && rowWidth >= (u64)width) AddRow(rows, length);
}

void
LayoutRowsFree(LayoutRows *rows)
{
  free(rows->starts);
  *rows = (LayoutRows){};
}

LayoutRow
LayoutGetRow(const LayoutRows *rows, u64 index, u64 length)
{
  bool last = index + 1 >= rows->count;
  u64 end = last ? length : rows->starts[index + 1];
  return (LayoutRow){rows->starts[index], end, last};
}

u64
LayoutRowOf(const LayoutRows *rows, u64 byte)
{
  u64 low = 0;
  u64 high = rows->count;

  // the last row starting at or before byte.
  while (high - low > 1) {
    u64 middle = low + (high - low) / 2;
    if (rows->starts[middle] <= byte) low = middle;
    else high = middle;
  }

  return low;
}

u64
LayoutByteAt(const char *text, u64 length, LayoutRow row, u64 column)
{
  LayoutIterator iterator = LayoutBeginAt(text, length, row.start);
  u64 target = iterator.column + column;
  LayoutCluster cluster;

  // the clusters ending before the target column are passed over, and the next one in
  // the row holds it.
  u64 rowEnd = row.last ? length : row.end;
  Seek(&iterator, rowEnd, target);
  if (LayoutNext(&iterator, &cluster) && cluster.start < rowEnd) return cluster.start;
  if (row.last) return row.end;

  // past the end of a row that wraps, the position stays on its final cluster.
  iterator = LayoutBeginAt(text, length, row.end > 0 ? row.end - 1 : 0);
  return iterator.at > row.start ? iterator.at : row.start;
}

void
LayoutShorten(const char *text, u64 length, i32 width, StrBuf *output)
{
  u64 total = LayoutColumn(text, length, length);

  if (total <= (u64)width) {
    StrBufAppend(output, text, length);
    return;
  }

  // the ellipsis takes three columns, and the head gets the odd one.
  u64 available = width > 3 ? (u64)width - 3 : 0;
  u64 tailWidth = available / 2;
  u64 headWidth = available - tailWidth;
  u64 headEnd = 0;
  u64 tailStart = length;
  bool tailFound = false;

  LayoutIterator iterator = LayoutBegin(text, length);
  LayoutCluster cluster;

  while (LayoutNext(&iterator, &cluster)) {
    if (cluster.column + cluster.width <= headWidth) headEnd = cluster.end;

    if (!tailFound && total - cluster.column <= tailWidth) {
      tailStart = cluster.start;
      tailFound = true;
    }
  }

  StrBufAppend(output, text, headEnd);
  if (width >= 3) StrBufAppendString(output, ellipsis);
  StrBufAppend(output, text + tailStart, length - tailStart);
}

void
LayoutCut(const char *text, u64 length, i32 width, StrBuf *output)
{
  // the clusters that fit, and those that leave a column for the ellipsis.
  LayoutIterator iterator = LayoutBegin(text, length);
  LayoutCluster cluster;
  u64 room = width > 0 ? (u64)width : 0;
  u64 cut = 0;

  while (LayoutNext(&iterator, &cluster)) {
    if (cluster.column + cluster.width > room) {
      StrBufAppend(output, text, cut);
      if (room) StrBufAppendString(output, "\u2026");
      return;
    }

    if (cluster.column + cluster.width < room) cut = cluster.end;
  }

  StrBufAppend(output, text, length);
}
