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

// true when a match shares text with what a cursor holds (or a cursor sits inside it).
static bool
RangesOverlap(Range match, Range held)
{
  bool before = PositionCompare(match.end, held.start) <= 0;
  bool after = PositionCompare(match.start, held.end) >= 0;
  return !before && !after;
}

// true when range overlaps a cursor other than the one at index skip.
static bool
OverlapsAny(const CursorSet *set, Range range, u32 skip)
{
  for (u32 index = 0; index < set->count; index++) {
    if (index != skip && RangesOverlap(range, CursorRange(&set->cursors[index]))) return true;
  }

  return false;
}

// what counts as an occurrence of the primary selection: the matches the document
// highlights when the selection is one of them, or else the selected text, matching case
// and as a whole word when it is one.
typedef struct Occurrences {
  MatchSet set;

  // the pattern of the selected text, which set points to when it is used.
  Pattern pattern;
} Occurrences;

static bool
FindOccurrences(View *view, MatchSet marks, Occurrences *occurrences, StrBuf *message)
{
  Range selection = CursorRange(CursorSetPrimary(&view->cursors));
  bool oneLine = selection.start.line == selection.end.line;
  PatternMatch match;

  if (marks.kind != MatchKindNone && oneLine) {
    u64 line = selection.start.line;
    bool found = MatchInLine(&marks, view->buffer, line, selection.start.byte, &match);

    if (found && match.start == selection.start.byte && match.end == selection.end.byte) {
      occurrences->set = marks;
      return true;
    }
  }

  if (!oneLine) {
    StrBufAppendString(message, "the selection spans more than one line");
    return false;
  }

  const StrBuf *line = BufferLine(view->buffer, selection.start.line);
  StrBuf source = {};
  u64 length = selection.end.byte - selection.start.byte;
  PatternEscape(line->data + selection.start.byte, length, &source);

  PatternOptions options = {
    .caseSensitive = true,
    .wholeWord = IsWholeWord(view->buffer, selection)
  };

  PatternSet(&occurrences->pattern, source.data, source.length, options);
  occurrences->set = (MatchSet){.kind = MatchKindSearch, .pattern = &occurrences->pattern};
  StrBufFree(&source);
  return true;
}

// finds the next occurrence after the primary selection that no cursor but the primary
// one holds, going round the end of the buffer until the search comes back to the primary
// selection.
static bool
NextOccurrence(View *view, const MatchSet *set, Range *found, bool *wrapped)
{
  const CursorSet *cursors = &view->cursors;
  Range selection = CursorRange(&cursors->cursors[cursors->primary]);
  Position from = selection.end;
  bool passedEnd = false;
  Range match;

  while (MatchFind(set, view->buffer, from, false, &match, wrapped)) {
    // a second time round would mean the selection itself no longer matches.
    if (passedEnd && *wrapped) break;

    passedEnd = passedEnd || *wrapped;
    if (passedEnd && PositionCompare(match.start, selection.start) >= 0) break;

    if (!OverlapsAny(cursors, match, cursors->primary)) {
      *found = match;
      *wrapped = passedEnd;
      return true;
    }

    from = match.end;
  }

  return false;
}

// with no selection at the primary cursor, selects the word at every cursor first.
// returns false when the primary cursor has no word.
static bool
SelectFirst(View *view, StrBuf *message)
{
  if (CursorHasSelection(CursorSetPrimary(&view->cursors)) || SelectWords(view)) return true;

  StrBufAppendString(message, "no word under the cursor");
  return false;
}

void
MultiCursorAddNext(View *view, MatchSet marks, StrBuf *message)
{
  // the first press only selects the word.
  if (!CursorHasSelection(CursorSetPrimary(&view->cursors))) {
    SelectFirst(view, message);
    return;
  }

  Occurrences occurrences = {};
  if (!FindOccurrences(view, marks, &occurrences, message)) return;

  Range match;
  bool wrapped;

  if (NextOccurrence(view, &occurrences.set, &match, &wrapped)) {
    Cursor cursor = {match.start, match.end, ViewRowColumn(view, match.end)};
    CursorSetAdd(&view->cursors, cursor);
    CursorSetMerge(&view->cursors);
    ViewScrollToCursor(view);
    if (wrapped) StrBufAppendString(message, "wrapped to the start");
  } else {
    StrBufAppendString(message, "no other occurrence");
  }

  PatternFree(&occurrences.pattern);
}

void
MultiCursorSkip(View *view, MatchSet marks, StrBuf *message)
{
  if (!SelectFirst(view, message)) return;

  Occurrences occurrences = {};
  if (!FindOccurrences(view, marks, &occurrences, message)) return;

  Range match;
  bool wrapped;

  if (NextOccurrence(view, &occurrences.set, &match, &wrapped)) {
    // the primary cursor moves to the occurrence, and stays primary.
    Cursor *primary = CursorSetPrimary(&view->cursors);
    *primary = (Cursor){match.start, match.end, ViewRowColumn(view, match.end)};
    CursorSetMerge(&view->cursors);
    ViewScrollToCursor(view);
    if (wrapped) StrBufAppendString(message, "wrapped to the start");
  } else {
    StrBufAppendString(message, "no other occurrence");
  }

  PatternFree(&occurrences.pattern);
}

void
MultiCursorAddAll(View *view, MatchSet marks, StrBuf *message)
{
  if (!SelectFirst(view, message)) return;

  Occurrences occurrences = {};
  if (!FindOccurrences(view, marks, &occurrences, message)) return;

  // the cursors are sorted and apart once merged, and the matches come in order, so one
  // walk through both passes over the matches that a cursor holds.
  CursorSet *set = &view->cursors;
  CursorSetMerge(set);
  u32 existing = set->count;
  u32 primary = set->primary;
  u32 next = 0;
  u64 count = BufferLineCount(view->buffer);
  PatternMatch found;

  for (u64 line = 0; line < count; line++) {
    u64 from = 0;

    while (MatchInLine(&occurrences.set, view->buffer, line, from, &found)) {
      Range match = {{line, found.start}, {line, found.end}};
      from = found.end;

      // cursors that end before the match end before every later one too.
      while (next < existing) {
        Range held = CursorRange(&set->cursors[next]);
        if (PositionCompare(held.end, match.start) > 0) break;
        next++;
      }

      if (next < existing && RangesOverlap(match, CursorRange(&set->cursors[next]))) continue;

      CursorSetAdd(set, (Cursor){match.start, match.end, ViewRowColumn(view, match.end)});
    }
  }

  u32 added = set->count - existing;
  set->primary = primary;
  CursorSetMerge(set);
  ViewScrollToCursor(view);

  if (added) StrBufAppendFormat(message, "%u cursors", set->count);
  else StrBufAppendString(message, "no other occurrence");
  PatternFree(&occurrences.pattern);
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
