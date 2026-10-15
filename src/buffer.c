#include "buffer.h"

#include <stdlib.h>
#include <string.h>

#include "memory.h"

static void
FreeLines(Buffer *buffer, u64 first, u64 count)
{
  for (u64 index = first; index < first + count; index++) StrBufFree(&buffer->lines[index]);
}

static void
ReserveLines(Buffer *buffer, u64 count)
{
  if (count <= buffer->lineCapacity) return;

  u64 capacity = buffer->lineCapacity ? buffer->lineCapacity : 16;
  while (capacity < count) capacity *= 2;

  buffer->lines = MemoryResize(buffer->lines, capacity, sizeof(StrBuf));
  buffer->lineCapacity = capacity;
}

void
BufferInit(Buffer *buffer)
{
  BufferSetText(buffer, "", 0);
}

// the first newline from at up to end, or nullptr.
static const char *
FindNewline(const char *at, const char *end)
{
  if (at >= end) return nullptr;
  return memchr(at, '\n', (u64)(end - at));
}

static u64
CountLines(const char *text, u64 length)
{
  u64 count = 1;
  const char *end = text + length;

  for (const char *at = text; (at = FindNewline(at, end)); at++) count++;
  return count;
}

void
BufferSetText(Buffer *buffer, const char *text, u64 length)
{
  FreeLines(buffer, 0, buffer->lineCount);

  // size the array exactly, since loaded files are often never edited.
  u64 count = CountLines(text, length);

  if (count > buffer->lineCapacity) {
    buffer->lines = MemoryResize(buffer->lines, count, sizeof(StrBuf));
    buffer->lineCapacity = count;
  }

  const char *end = text + length;
  const char *start = text;

  for (u64 index = 0; index < count; index++) {
    const char *newline = FindNewline(start, end);
    const char *lineEnd = newline ? newline : end;

    buffer->lines[index] = (StrBuf){};
    StrBufAppend(&buffer->lines[index], start, (u64)(lineEnd - start));
    start = lineEnd + 1;
  }

  buffer->lineCount = count;
  buffer->revision++;
}

void
BufferSwapText(Buffer *buffer, Buffer *other)
{
  Buffer swapped = *buffer;
  buffer->lines = other->lines;
  buffer->lineCount = other->lineCount;
  buffer->lineCapacity = other->lineCapacity;
  other->lines = swapped.lines;
  other->lineCount = swapped.lineCount;
  other->lineCapacity = swapped.lineCapacity;
  buffer->revision++;
  other->revision++;
}

void
BufferFree(Buffer *buffer)
{
  FreeLines(buffer, 0, buffer->lineCount);
  free(buffer->lines);
  free(buffer->listeners);
  StrBufFree(&buffer->removed);
  *buffer = (Buffer){};
}

void
BufferListen(Buffer *buffer, BufferListener listener, void *context)
{
  u32 count = buffer->listenerCount + 1;
  buffer->listeners = MemoryResize(buffer->listeners, count, sizeof(BufferListenerEntry));
  buffer->listeners[buffer->listenerCount] = (BufferListenerEntry){listener, context};
  buffer->listenerCount = count;
}

void
BufferUnlisten(Buffer *buffer, BufferListener listener, void *context)
{
  for (u32 index = 0; index < buffer->listenerCount; index++) {
    BufferListenerEntry *entry = &buffer->listeners[index];
    if (entry->listener != listener || entry->context != context) continue;

    u32 after = buffer->listenerCount - index - 1;
    memmove(entry, entry + 1, after * sizeof(BufferListenerEntry));
    buffer->listenerCount--;
    return;
  }
}

Position
BufferEnd(const Buffer *buffer)
{
  u64 last = buffer->lineCount - 1;
  return (Position){last, buffer->lines[last].length};
}

Position
BufferClamp(const Buffer *buffer, Position position)
{
  if (position.line >= buffer->lineCount) return BufferEnd(buffer);

  u64 length = buffer->lines[position.line].length;
  if (position.byte > length) position.byte = length;
  return position;
}

i32
PositionCompare(Position a, Position b)
{
  if (a.line != b.line) return a.line < b.line ? -1 : 1;
  if (a.byte != b.byte) return a.byte < b.byte ? -1 : 1;
  return 0;
}

Position
BufferAdjust(Position position, const BufferChange *change)
{
  if (PositionCompare(position, change->start) < 0) return position;
  if (PositionCompare(position, change->oldEnd) < 0) return change->start;

  Position oldEnd = change->oldEnd;
  Position newEnd = change->newEnd;
  // a position on the last replaced line keeps its distance from the end of the change.
  if (position.line == oldEnd.line) {
    return (Position){newEnd.line, newEnd.byte + position.byte - oldEnd.byte};
  }

  return (Position){position.line - oldEnd.line + newEnd.line, position.byte};
}

Position
BufferTextEnd(Position start, const char *text, u64 length)
{
  const char *end = text + length;
  const char *lastNewline = nullptr;
  u64 newlines = 0;

  for (const char *at = text; (at = FindNewline(at, end)); at++) {
    lastNewline = at;
    newlines++;
  }

  if (!newlines) return (Position){start.line, start.byte + length};
  return (Position){start.line + newlines, (u64)(end - lastNewline - 1)};
}

static Range
NormaliseRange(const Buffer *buffer, Range range)
{
  range.start = BufferClamp(buffer, range.start);
  range.end = BufferClamp(buffer, range.end);
  if (PositionCompare(range.start, range.end) <= 0) return range;
  return (Range){range.end, range.start};
}

void
BufferGetText(const Buffer *buffer, Range range, StrBuf *text)
{
  range = NormaliseRange(buffer, range);
  Position start = range.start;
  Position end = range.end;

  for (u64 index = start.line; index <= end.line; index++) {
    const StrBuf *line = &buffer->lines[index];
    u64 from = index == start.line ? start.byte : 0;
    u64 to = index == end.line ? end.byte : line->length;

    StrBufAppend(text, line->data + from, to - from);
    if (index < end.line) StrBufAppendByte(text, '\n');
  }
}

// replaces the lines from range.start.line to range.end.line with the lines of text.
// the head of the first line and the tail of the last line are kept around the new text.
static Position
ReplaceLines(Buffer *buffer, Range range, const char *text, u64 length)
{
  Position start = range.start;
  Position end = range.end;
  u64 newCount = CountLines(text, length);
  u64 oldCount = end.line - start.line + 1;

  // keep the tail of the last line before it is freed or overwritten.
  const StrBuf *last = &buffer->lines[end.line];
  StrBuf tail = {};
  StrBufAppend(&tail, last->data + end.byte, last->length - end.byte);

  // the first line keeps its head; the other old lines go.
  FreeLines(buffer, start.line + 1, oldCount - 1);
  ReserveLines(buffer, buffer->lineCount - oldCount + newCount);

  StrBuf *lines = buffer->lines;
  u64 after = buffer->lineCount - end.line - 1;
  memmove(&lines[start.line + newCount], &lines[end.line + 1], after * sizeof(StrBuf));
  buffer->lineCount = buffer->lineCount - oldCount + newCount;

  StrBuf *first = &lines[start.line];
  first->length = start.byte;
  if (first->data) first->data[start.byte] = '\0';

  const char *textEnd = text + length;
  const char *piece = text;

  for (u64 index = 0; index < newCount; index++) {
    const char *newline = FindNewline(piece, textEnd);
    const char *pieceEnd = newline ? newline : textEnd;
    StrBuf *line = &lines[start.line + index];

    if (index > 0) *line = (StrBuf){};
    StrBufAppend(line, piece, (u64)(pieceEnd - piece));
    piece = pieceEnd + 1;
  }

  StrBuf *lastNew = &lines[start.line + newCount - 1];
  Position newEnd = {start.line + newCount - 1, lastNew->length};
  StrBufAppend(lastNew, tail.data, tail.length);
  StrBufFree(&tail);
  return newEnd;
}

Position
BufferReplace(Buffer *buffer, Range range, const char *text, u64 length)
{
  range = NormaliseRange(buffer, range);
  Position start = range.start;
  Position end = range.end;

  StrBufClear(&buffer->removed);
  BufferGetText(buffer, range, &buffer->removed);

  Position newEnd;
  bool singleLine = start.line == end.line && !FindNewline(text, text + length);

  if (singleLine) {
    // typing and deleting within a line is the common case and needs no line shuffling.
    StrBuf *line = &buffer->lines[start.line];
    StrBufRemove(line, start.byte, end.byte - start.byte);
    StrBufInsert(line, start.byte, text, length);
    newEnd = (Position){start.line, start.byte + length};
  } else {
    newEnd = ReplaceLines(buffer, range, text, length);
  }

  buffer->revision++;

  BufferChange change = {
    .start = start,
    .oldEnd = end,
    .newEnd = newEnd,
    .oldText = buffer->removed.data ? buffer->removed.data : "",
    .oldLength = buffer->removed.length,
    .newText = text ? text : "",
    .newLength = length
  };

  for (u32 index = 0; index < buffer->listenerCount; index++) {
    BufferListenerEntry *entry = &buffer->listeners[index];
    entry->listener(entry->context, &change);
  }

  return newEnd;
}
