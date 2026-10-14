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

u64
LayoutColumn(const char *text, u64 length, u64 byte)
{
  LayoutIterator iterator = LayoutBegin(text, length);
  LayoutCluster cluster;

  while (LayoutNext(&iterator, &cluster)) {
    if (cluster.end > byte) return cluster.column;
  }

  return iterator.column;
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

  LayoutIterator iterator = LayoutBegin(text, length);
  LayoutCluster cluster;
  u64 rowStart = 0;
  u64 rowWidth = 0;

  // the byte after the last space or tab in the row, and the row width up to it.
  u64 breakAt = 0;
  u64 breakWidth = 0;

  while (LayoutNext(&iterator, &cluster)) {
    while (rowWidth > 0 && rowWidth + cluster.width > (u64)width) {
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
  LayoutIterator iterator = LayoutBegin(text, length);
  LayoutCluster cluster;
  u64 rowColumn = 0;
  u64 lastStart = row.start;
  bool started = false;

  while (LayoutNext(&iterator, &cluster)) {
    if (cluster.start < row.start) continue;
    if (cluster.start >= row.end) break;

    if (!started) {
      rowColumn = cluster.column;
      started = true;
    }

    if (cluster.column + cluster.width > rowColumn + column) return cluster.start;
    lastStart = cluster.start;
  }

  return row.last ? row.end : lastStart;
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
