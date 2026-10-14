#include "move.h"

#include "utf8.h"

enum CharacterClass {
  CharacterClassSpace,
  CharacterClassWord,
  CharacterClassPunctuation
};

static const StrBuf *
LineOf(const View *view, u64 line)
{
  return BufferLine(view->buffer, line);
}

static enum CharacterClass
ClassAt(const StrBuf *text, u64 byte)
{
  u32 codepoint;
  Utf8Decode(text->data + byte, text->length - byte, &codepoint);

  if (codepoint == ' ' || codepoint == '\t') return CharacterClassSpace;

  // any non-ascii character counts as part of a word.
  bool letter = (codepoint | 0x20) >= 'a' && (codepoint | 0x20) <= 'z';
  bool digit = codepoint >= '0' && codepoint <= '9';
  bool word = letter || digit || codepoint == '_' || codepoint >= 0x80;
  return word ? CharacterClassWord : CharacterClassPunctuation;
}

static Position
Left(View *view, Position at)
{
  if (at.byte > 0) {
    const StrBuf *text = LineOf(view, at.line);
    return (Position){at.line, Utf8PreviousBoundary(text->data, text->length, at.byte)};
  }

  if (at.line == 0) return at;
  return (Position){at.line - 1, LineOf(view, at.line - 1)->length};
}

static Position
Right(View *view, Position at)
{
  const StrBuf *text = LineOf(view, at.line);
  if (at.byte < text->length) {
    return (Position){at.line, Utf8NextBoundary(text->data, text->length, at.byte)};
  }

  if (at.line + 1 >= BufferLineCount(view->buffer)) return at;
  return (Position){at.line + 1, 0};
}

static Position
WordRight(View *view, Position at)
{
  const StrBuf *text = LineOf(view, at.line);
  if (at.byte >= text->length) return Right(view, at);

  // skip blanks, then the run of word or punctuation characters after them.
  u64 byte = at.byte;

  while (byte < text->length && ClassAt(text, byte) == CharacterClassSpace) {
    byte = Utf8NextBoundary(text->data, text->length, byte);
  }

  if (byte < text->length) {
    enum CharacterClass class = ClassAt(text, byte);

    while (byte < text->length && ClassAt(text, byte) == class) {
      byte = Utf8NextBoundary(text->data, text->length, byte);
    }
  }

  return (Position){at.line, byte};
}

static Position
WordLeft(View *view, Position at)
{
  if (at.byte == 0) return Left(view, at);

  const StrBuf *text = LineOf(view, at.line);
  u64 byte = at.byte;
  u64 previous = Utf8PreviousBoundary(text->data, text->length, byte);

  while (byte > 0 && ClassAt(text, previous) == CharacterClassSpace) {
    byte = previous;
    if (byte > 0) previous = Utf8PreviousBoundary(text->data, text->length, byte);
  }

  if (byte > 0) {
    enum CharacterClass class = ClassAt(text, previous);

    while (byte > 0 && ClassAt(text, previous) == class) {
      byte = previous;
      if (byte > 0) previous = Utf8PreviousBoundary(text->data, text->length, byte);
    }
  }

  return (Position){at.line, byte};
}

static Position
LineStart(View *view, Position at)
{
  // the first non-blank character, or the line start when already there.
  const StrBuf *text = LineOf(view, at.line);
  u64 indent = 0;

  while (indent < text->length && (text->data[indent] == ' ' || text->data[indent] == '\t')) {
    indent++;
  }

  return (Position){at.line, at.byte == indent ? 0 : indent};
}

static Position
Vertical(View *view, const Cursor *cursor, i64 rows)
{
  u64 moved;
  ViewRow row = ViewRowOf(view, cursor->head);
  ViewRow target = ViewStep(view, row, rows, &moved);

  // with nowhere left to go, the cursor goes to the very start or end.
  if (moved == 0) return rows < 0 ? (Position){0, 0} : BufferEnd(view->buffer);
  return ViewPositionAt(view, target, cursor->desiredColumn);
}

static i64
PageRows(const View *view)
{
  return view->height > 2 ? view->height - 1 : 1;
}

static Position
Target(View *view, const Cursor *cursor, enum Move move)
{
  Position head = cursor->head;

  switch (move) {
  case MoveUp: return Vertical(view, cursor, -1);
  case MoveDown: return Vertical(view, cursor, 1);
  case MoveLeft: return Left(view, head);
  case MoveRight: return Right(view, head);
  case MoveLineStart: return LineStart(view, head);
  case MoveLineEnd: return (Position){head.line, LineOf(view, head.line)->length};
  case MovePageUp: return Vertical(view, cursor, -PageRows(view));
  case MovePageDown: return Vertical(view, cursor, PageRows(view));
  case MoveWordLeft: return WordLeft(view, head);
  case MoveWordRight: return WordRight(view, head);
  case MoveFileStart: return (Position){0, 0};
  case MoveFileEnd: return BufferEnd(view->buffer);
  }

  return head;
}

static bool
IsVertical(enum Move move)
{
  return move == MoveUp || move == MoveDown || move == MovePageUp || move == MovePageDown;
}

void
MoveCursors(View *view, enum Move move, bool extend)
{
  CursorSet *set = &view->cursors;

  for (u32 index = 0; index < set->count; index++) {
    Cursor *cursor = &set->cursors[index];
    cursor->head = Target(view, cursor, move);
    if (!extend) cursor->anchor = cursor->head;

    // vertical movement keeps aiming for the same column.
    if (!IsVertical(move)) cursor->desiredColumn = ViewRowColumn(view, cursor->head);
  }

  // a page move scrolls the view with the cursor, so the cursor keeps its screen row.
  u64 moved;
  if (move == MovePageUp) view->top = ViewStep(view, view->top, -PageRows(view), &moved);
  if (move == MovePageDown) view->top = ViewStep(view, view->top, PageRows(view), &moved);

  ViewScrollToCursor(view);
}
