#include "move.h"

#include "bracket.h"
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
  return Utf8IsWord(codepoint) ? CharacterClassWord : CharacterClassPunctuation;
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

static bool
IsWordAt(const StrBuf *line, u64 at)
{
  if (at >= line->length) return false;

  u32 codepoint;
  Utf8Decode(line->data + at, line->length - at, &codepoint);
  return Utf8IsWord(codepoint);
}

bool
MoveWordAround(const StrBuf *line, u64 at, u64 *start, u64 *end)
{
  *start = at;
  *end = at;

  while (*start > 0 && IsWordAt(line, Utf8PreviousStart(line->data, *start))) {
    *start = Utf8PreviousStart(line->data, *start);
  }

  while (IsWordAt(line, *end)) {
    u32 codepoint;
    *end += Utf8Decode(line->data + *end, line->length - *end, &codepoint);
  }

  return *end > *start;
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

Position
MovePosition(View *view, const Cursor *cursor, enum Move move)
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
    cursor->head = MovePosition(view, cursor, move);
    if (!extend) cursor->anchor = cursor->head;

    // vertical movement keeps aiming for the same column.
    if (!IsVertical(move)) cursor->desiredColumn = ViewRowColumn(view, cursor->head);
  }

  // cursors that moved onto each other become one.
  CursorSetMerge(set);

  // a page move scrolls the view with the cursor, so the cursor keeps its screen row.
  u64 moved;
  if (move == MovePageUp) view->top = ViewStep(view, view->top, -PageRows(view), &moved);
  if (move == MovePageDown) view->top = ViewStep(view, view->top, PageRows(view), &moved);

  ViewScrollToCursor(view);
}

void
MoveSelect(View *view, Position anchor, Position head)
{
  CursorSetCollapse(&view->cursors);
  Cursor *cursor = CursorSetPrimary(&view->cursors);
  cursor->anchor = anchor;
  cursor->head = head;
  cursor->desiredColumn = ViewRowColumn(view, head);
  ViewScrollToCursor(view);
}

void
MoveSelectAll(View *view)
{
  MoveSelect(view, (Position){0, 0}, BufferEnd(view->buffer));
}

bool
MoveToMatchingBracket(View *view)
{
  Position head = CursorSetPrimary(&view->cursors)->head;
  Position match;
  bool found = BracketMatch(view->buffer, head, &match);

  // a bracket just before the cursor counts too, as it is where typing one leaves it.
  if (!found && head.byte > 0) {
    found = BracketMatch(view->buffer, (Position){head.line, head.byte - 1}, &match);
  }

  if (found) MoveSelect(view, match, match);
  return found;
}

bool
MoveSelectBrackets(View *view)
{
  Range selection = CursorRange(CursorSetPrimary(&view->cursors));
  Range pair;
  if (!BracketEnclosing(view->buffer, selection, &pair)) return false;

  // the text inside the brackets, or the brackets too when that is already selected.
  Position inside = {pair.start.line, pair.start.byte + 1};
  bool insideSelected = PositionCompare(selection.start, inside) == 0 &&
    PositionCompare(selection.end, pair.end) == 0;

  Position after = {pair.end.line, pair.end.byte + 1};

  if (insideSelected) MoveSelect(view, pair.start, after);
  else MoveSelect(view, inside, pair.end);
  return true;
}

void
MoveGoTo(View *view, u64 line, u64 column)
{
  u64 lastLine = BufferLineCount(view->buffer) - 1;
  if (line > lastLine) line = lastLine;

  const StrBuf *text = BufferLine(view->buffer, line);
  LayoutRow whole = {0, text->length, true};
  Position position = {line, LayoutByteAt(text->data, text->length, whole, column)};
  MoveSelect(view, position, position);
}
