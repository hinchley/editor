#pragma once

// the text of one buffer: a flat array of lines, each a growable utf-8 byte string
// without its line break.
// a buffer always has at least one line, and joining the lines with "\n" gives its text.
// every change goes through BufferReplace, which tells the listeners about it, so
// cursors, highlighting and undo all follow edits from one place.
#include "strbuf.h"
#include "types.h"

// a place in the text: a line index and a byte offset within that line.
typedef struct Position {
  u64 line;
  u64 byte;
} Position;

// the text from start up to (not including) end.
typedef struct Range {
  Position start;
  Position end;
} Range;

// one replacement, as passed to listeners.
// the text pointers are only valid while the listener runs, and a listener must not
// change the buffer.
typedef struct BufferChange {
  Position start;
  Position oldEnd;
  Position newEnd;
  const char *oldText;
  u64 oldLength;
  const char *newText;
  u64 newLength;
} BufferChange;

typedef void (*BufferListener)(void *context, const BufferChange *change);

typedef struct BufferListenerEntry {
  BufferListener listener;
  void *context;
} BufferListenerEntry;

typedef struct Buffer {
  StrBuf *lines;
  u64 lineCount;
  u64 lineCapacity;

  BufferListenerEntry *listeners;
  u32 listenerCount;

  // holds the replaced text while listeners run.
  StrBuf removed;
} Buffer;

// makes buffer a single empty line.
// a zeroed Buffer must be initialised with this or BufferSetText before use.
void
BufferInit(Buffer *buffer);

// replaces all the text without telling listeners, for loading.
// text is split on "\n"; any "\r" is kept as content.
void
BufferSetText(Buffer *buffer, const char *text, u64 length);

// releases the text and listeners and leaves a zeroed Buffer.
void
BufferFree(Buffer *buffer);

void
BufferListen(Buffer *buffer, BufferListener listener, void *context);

void
BufferUnlisten(Buffer *buffer, BufferListener listener, void *context);

static inline u64
BufferLineCount(const Buffer *buffer)
{
  return buffer->lineCount;
}

static inline const StrBuf *
BufferLine(const Buffer *buffer, u64 line)
{
  return &buffer->lines[line];
}

// the position after the last byte.
Position
BufferEnd(const Buffer *buffer);

// moves a position inside the text: the line onto the last line and the byte onto the
// line's end.
// it does not snap to a code point boundary.
Position
BufferClamp(const Buffer *buffer, Position position);

// -1, 0 or 1 as a is before, equal to or after b.
i32
PositionCompare(Position a, Position b);

// appends the text of range, with lines joined by "\n".
void
BufferGetText(const Buffer *buffer, Range range, StrBuf *text);

// replaces range with text (lines separated by "\n") and returns the end of the new text.
// the range is clamped to the buffer and may be given in either order.
// insertion is an empty range and deletion is empty text.
Position
BufferReplace(Buffer *buffer, Range range, const char *text, u64 length);
