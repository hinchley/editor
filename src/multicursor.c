#include "multicursor.h"

#include "match.h"
#include "move.h"
#include "pattern.h"

// selects the word at every cursor that has no selection and sits on or just after one.
// returns false, changing nothing, when the primary cursor has no word.
static bool
SelectWords(View *view)
{
  CursorSet *set = &view->cursors;
  Position head = CursorSetPrimary(set)->head;
  u64 start;
  u64 end;
  const StrBuf *primaryLine = BufferLine(view->buffer, head.line);
  if (!MoveWordAround(primaryLine, head.byte, &start, &end)) return false;

  for (u32 index = 0; index < set->count; index++) {
    Cursor *cursor = &set->cursors[index];
    if (CursorHasSelection(cursor)) continue;

    const StrBuf *line = BufferLine(view->buffer, cursor->head.line);
    if (!MoveWordAround(line, cursor->head.byte, &start, &end)) continue;

    cursor->anchor = (Position){cursor->head.line, start};
    cursor->head = (Position){cursor->head.line, end};
    cursor->desiredColumn = ViewRowColumn(view, cursor->head);
  }

  CursorSetMerge(set);
  return true;
}

// true when a range within one line is exactly one word.
static bool
IsWholeWord(const Buffer *buffer, Range range)
{
  const StrBuf *line = BufferLine(buffer, range.start.line);
  u64 start;
  u64 end;
  bool found = MoveWordAround(line, range.end.byte, &start, &end);
  return found && start == range.start.byte && end == range.end.byte;
}

static bool
OverlapsAny(const CursorSet *set, Range range)
{
  for (u32 index = 0; index < set->count; index++) {
    Range held = CursorRange(&set->cursors[index]);
    bool before = PositionCompare(range.end, held.start) <= 0;
    bool after = PositionCompare(range.start, held.end) >= 0;
    if (!before && !after) return true;
  }

  return false;
}

void
MultiCursorAddNext(View *view, StrBuf *message)
{
  CursorSet *set = &view->cursors;
  Range selection = CursorRange(CursorSetPrimary(set));

  if (!CursorHasSelection(CursorSetPrimary(set))) {
    if (!SelectWords(view)) StrBufAppendString(message, "no word under the cursor");
    return;
  }

  if (selection.start.line != selection.end.line) {
    StrBufAppendString(message, "the selection spans more than one line");
    return;
  }

  const StrBuf *line = BufferLine(view->buffer, selection.start.line);
  StrBuf source = {};
  u64 length = selection.end.byte - selection.start.byte;
  PatternEscape(line->data + selection.start.byte, length, &source);

  Pattern pattern = {};
  PatternOptions options = {
    .caseSensitive = true,
    .wholeWord = IsWholeWord(view->buffer, selection)
  };

  PatternSet(&pattern, source.data, source.length, options);
  MatchSet matches = {.kind = MatchKindSearch, .pattern = &pattern};

  // matches held by other cursors are passed over, until the search comes back round to
  // the primary selection.
  Position from = selection.end;
  bool passedEnd = false;
  bool added = false;
  Range match;
  bool wrapped;

  while (MatchFind(&matches, view->buffer, from, false, &match, &wrapped)) {
    // a second time round would mean the selection itself no longer matches.
    if (passedEnd && wrapped) break;

    passedEnd = passedEnd || wrapped;
    if (passedEnd && PositionCompare(match.start, selection.start) >= 0) break;

    if (!OverlapsAny(set, match)) {
      Cursor cursor = {match.start, match.end, 0};
      cursor.desiredColumn = ViewRowColumn(view, match.end);
      CursorSetAdd(set, cursor);
      CursorSetMerge(set);
      ViewScrollToCursor(view);
      added = true;
      if (passedEnd) StrBufAppendString(message, "wrapped to the start");
      break;
    }

    from = match.end;
  }

  if (!added) StrBufAppendString(message, "no other occurrence");
  PatternFree(&pattern);
  StrBufFree(&source);
}

bool
MultiCursorAddRow(View *view, bool up)
{
  CursorSet *set = &view->cursors;
  CursorSetMerge(set);
  Cursor *edge = &set->cursors[up ? 0 : set->count - 1];

  // a selection is stepped over from its outer end, or the new cursor would land inside it.
  Range range = CursorRange(edge);
  Position from = up ? range.start : range.end;

  u64 moved;
  ViewRow row = ViewStep(view, ViewRowOf(view, from), up ? -1 : 1, &moved);
  if (moved == 0) return false;

  Position position = ViewPositionAt(view, row, edge->desiredColumn);
  Cursor cursor = {position, position, edge->desiredColumn};
  CursorSetAdd(set, cursor);
  CursorSetMerge(set);
  ViewScrollToCursor(view);
  return true;
}
