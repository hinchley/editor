#pragma once

// how one line of text is laid out on screen: the display width of each grapheme cluster
// (with tabs expanded to the next tab stop) and, when wrapping, where the line breaks into
// visual rows.
// movement, scrolling and drawing all measure text through this module, so they agree.
#include "strbuf.h"
#include "types.h"

enum LayoutKind {
  // drawn as its own bytes.
  LayoutKindText,
  LayoutKindSpace,
  LayoutKindTab,

  // an ascii control character, drawn as ^X in two columns.
  LayoutKindControl,

  // an invalid byte, a c1 control character or a cluster with no width of its own.
  // drawn as U+FFFD in one column.
  LayoutKindInvalid
};

// one grapheme cluster: its bytes, the display column it starts at (from the start of the
// line) and its width.
typedef struct LayoutCluster {
  u64 start;
  u64 end;
  u64 column;
  u32 width;
  enum LayoutKind kind;
} LayoutCluster;

// walks the clusters of a line from its start.
typedef struct LayoutIterator {
  const char *text;
  u64 length;
  u64 at;
  u64 column;
} LayoutIterator;

// the start of every visual row of a line, in byte offsets.
// the first row starts at 0, and there is always at least one row.
typedef struct LayoutRows {
  u64 *starts;
  u64 count;
  u64 capacity;
} LayoutRows;

// one visual row: its bytes, and whether it is the last row of its line.
typedef struct LayoutRow {
  u64 start;
  u64 end;
  bool last;
} LayoutRow;

LayoutIterator
LayoutBegin(const char *text, u64 length);

// an iterator starting at the cluster that holds byte (or at the end of the line).
LayoutIterator
LayoutBeginAt(const char *text, u64 length, u64 byte);

// an iterator starting at the cluster that covers display column (or at the end of the
// line).
LayoutIterator
LayoutBeginAtColumn(const char *text, u64 length, u64 column);

// reads the next cluster, or returns false at the end of the line.
bool
LayoutNext(LayoutIterator *iterator, LayoutCluster *cluster);

// display column (from 0) at which the cluster holding byte starts.
// byte may be the line length, giving the width of the whole line.
u64
LayoutColumn(const char *text, u64 length, u64 byte);

// splits a line into visual rows no wider than width - 1, leaving the last column free so
// the end of the line always has a column on its last row.
// a row breaks after its last space or tab that fits, or else before the cluster that
// does not fit.
// when a cluster too wide for that fills the last row, an empty row follows it.
// a width of 0 means no wrapping: the line is one row.
void
LayoutWrap(const char *text, u64 length, i32 width, LayoutRows *rows);

void
LayoutRowsFree(LayoutRows *rows);

LayoutRow
LayoutGetRow(const LayoutRows *rows, u64 index, u64 length);

// index of the row holding byte.
// a byte at a row break belongs to the row that starts there.
u64
LayoutRowOf(const LayoutRows *rows, u64 byte);

// the byte offset of the cluster at column, counted from the start of row.
// past the end of the row it gives the end of a last row, or the start of the final
// cluster of any other row (so the position stays on that row).
u64
LayoutByteAt(const char *text, u64 length, LayoutRow row, u64 column);

// appends text, shortened in the middle with "..." when it is wider than width columns.
void
LayoutShorten(const char *text, u64 length, i32 width, StrBuf *output);

// appends the start of text that fits in width columns, ending with "…" when the rest is
// cut off.
void
LayoutCut(const char *text, u64 length, i32 width, StrBuf *output);
