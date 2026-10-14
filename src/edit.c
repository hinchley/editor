#include "edit.h"

#include <stdlib.h>
#include <string.h>

#include "layout.h"
#include "memory.h"
#include "move.h"
#include "settings.h"

// the change one line gets from a line command: removed bytes at an offset replaced by
// text.
typedef struct LineEdit {
  u64 at;
  u64 removed;
  const char *text;
  u64 length;
} LineEdit;

typedef LineEdit (*LineEditor)(const EditContext *edit, const StrBuf *line);

// a run of lines, first to last inclusive.
typedef struct LineBlock {
  u64 first;
  u64 last;
} LineBlock;

// gives the range a cursor's command acts on.
typedef Range (*RangeOf)(const EditContext *edit, const Cursor *cursor);

static bool
IsBlank(char character)
{
  return character == ' ' || character == '\t';
}

static u64
IndentLength(const StrBuf *line)
{
  u64 length = 0;
  while (length < line->length && IsBlank(line->data[length])) length++;
  return length;
}

// enough spaces for any indentation step, which is at most one tab stop.
static constexpr char spaces[] = "                ";
static_assert((u64)tabWidth < sizeof spaces);

// one level of indentation.
static const char *
IndentUnit(const EditContext *edit, u64 *length)
{
  *length = edit->expandTab ? (u64)tabWidth : 1;
  return edit->expandTab ? spaces : "\t";
}

static void
AppendIndentUnit(const EditContext *edit, StrBuf *text)
{
  u64 length;
  const char *unit = IndentUnit(edit, &length);
  StrBufAppend(text, unit, length);
}

static void
Begin(const EditContext *edit, enum UndoKind kind)
{
  UndoBegin(edit->undo, kind, &edit->view->cursors);
}

static void
ResetDesiredColumns(View *view)
{
  CursorSet *set = &view->cursors;

  for (u32 index = 0; index < set->count; index++) {
    set->cursors[index].desiredColumn = ViewRowColumn(view, set->cursors[index].head);
  }
}

static void
End(const EditContext *edit)
{
  CursorSet *set = &edit->view->cursors;
  CursorSetMerge(set);
  ResetDesiredColumns(edit->view);
  UndoEnd(edit->undo, set);
}

// replaces range with text and leaves cursor index just after the new text.
static void
ReplaceAt(const EditContext *edit, u32 index, Range range, const char *text, u64 length)
{
  Cursor *cursor = &edit->view->cursors.cursors[index];

  // an empty replacement would only record an empty step in the history.
  if (PositionCompare(range.start, range.end) == 0 && length == 0) {
    CursorPlace(cursor, range.start);
    return;
  }

  Position end = BufferReplace(edit->view->buffer, range, text, length);
  CursorPlace(cursor, end);
}

static void
ReplaceEach(const EditContext *edit, enum UndoKind kind, RangeOf rangeOf)
{
  CursorSet *set = &edit->view->cursors;
  Begin(edit, kind);

  // earlier edits move the later cursors through the view's buffer listener.
  for (u32 index = 0; index < set->count; index++) {
    ReplaceAt(edit, index, rangeOf(edit, &set->cursors[index]), "", 0);
  }

  End(edit);
}

void
EditInsert(const EditContext *edit, const char *text, u64 length)
{
  CursorSet *set = &edit->view->cursors;
  Begin(edit, UndoKindTyping);

  for (u32 index = 0; index < set->count; index++) {
    ReplaceAt(edit, index, CursorRange(&set->cursors[index]), text, length);
  }

  End(edit);
}

// appends text with every "\r\n" turned into "\n".
static void
AppendNormalised(StrBuf *output, const char *text, u64 length)
{
  const char *end = text + length;
  const char *at = text;

  while (at < end) {
    const char *carriageReturn = memchr(at, '\r', (u64)(end - at));
    const char *stop = carriageReturn ? carriageReturn : end;
    StrBufAppend(output, at, (u64)(stop - at));
    if (!carriageReturn) break;

    bool lineEnd = carriageReturn + 1 < end && carriageReturn[1] == '\n';
    if (!lineEnd) StrBufAppendByte(output, '\r');
    at = carriageReturn + 1;
  }
}

void
EditPaste(const EditContext *edit, const char *text, u64 length, bool lineWise)
{
  StrBuf normal = {};
  AppendNormalised(&normal, text, length);
  const char *data = normal.data ? normal.data : "";

  CursorSet *set = &edit->view->cursors;
  Begin(edit, UndoKindOther);

  for (u32 index = 0; index < set->count; index++) {
    Cursor *cursor = &set->cursors[index];

    // whole lines go in above, and the cursor moves down with its own line.
    if (lineWise && !CursorHasSelection(cursor)) {
      Range lineStart = {{cursor->head.line, 0}, {cursor->head.line, 0}};
      BufferReplace(edit->view->buffer, lineStart, data, normal.length);
      continue;
    }

    ReplaceAt(edit, index, CursorRange(cursor), data, normal.length);
  }

  End(edit);
  StrBufFree(&normal);
}

// the last character before byte that is not a space or tab, or 0 when there is none.
static char
LastBefore(const StrBuf *line, u64 byte)
{
  while (byte > 0 && IsBlank(line->data[byte - 1])) byte--;
  return byte > 0 ? line->data[byte - 1] : '\0';
}

static char
CloserOf(char opener)
{
  switch (opener) {
  case '(': return ')';
  case '[': return ']';
  case '{': return '}';
  default: return '\0';
  }
}

static void
NewlineAt(const EditContext *edit, u32 index)
{
  Buffer *buffer = edit->view->buffer;
  Range range = CursorRange(&edit->view->cursors.cursors[index]);
  const StrBuf *line = BufferLine(buffer, range.start.line);
  u64 indent = IndentLength(line);
  if (indent > range.start.byte) indent = range.start.byte;

  StrBuf text = {};
  StrBufAppendByte(&text, '\n');
  StrBufAppend(&text, line->data, indent);
  char opener = LastBefore(line, range.start.byte);

  // a line with only blanks before the cursor is left empty, not with trailing blanks.
  if (indent == range.start.byte) range.start.byte = 0;

  // blanks after the cursor would otherwise add to the new line's indentation.
  const StrBuf *endLine = BufferLine(buffer, range.end.line);

  while (range.end.byte < endLine->length && IsBlank(endLine->data[range.end.byte])) {
    range.end.byte++;
  }

  char closer = CloserOf(opener);
  if (closer) AppendIndentUnit(edit, &text);
  Position cursor = {range.start.line + 1, text.length - 1};

  // between a pair of brackets, the closer goes on its own line below the cursor.
  bool between = closer && range.end.byte < endLine->length &&
    endLine->data[range.end.byte] == closer;

  if (between) {
    StrBufAppendByte(&text, '\n');
    StrBufAppend(&text, line->data, indent);
  }

  BufferReplace(buffer, range, text.data, text.length);
  CursorPlace(&edit->view->cursors.cursors[index], cursor);
  StrBufFree(&text);
}

void
EditNewline(const EditContext *edit)
{
  CursorSet *set = &edit->view->cursors;
  Begin(edit, UndoKindOther);
  for (u32 index = 0; index < set->count; index++) NewlineAt(edit, index);
  End(edit);
}

// the lines a cursor touches.
// a selection ending at the very start of a line does not touch that line.
static LineBlock
TouchedLines(const Cursor *cursor)
{
  Range range = CursorRange(cursor);
  LineBlock block = {range.start.line, range.end.line};
  if (block.last > block.first && range.end.byte == 0) block.last--;
  return block;
}

// the touched lines of every cursor (sorted by position) as runs of lines.
static LineBlock *
TouchedBlocks(const CursorSet *set, u32 *count)
{
  LineBlock *blocks = MemoryAllocate(set->count * sizeof(LineBlock));
  *count = 0;

  for (u32 index = 0; index < set->count; index++) {
    LineBlock block = TouchedLines(&set->cursors[index]);
    LineBlock *previous = *count ? &blocks[*count - 1] : nullptr;

    if (previous && block.first <= previous->last + 1) {
      if (block.last > previous->last) previous->last = block.last;
      continue;
    }

    blocks[(*count)++] = block;
  }

  return blocks;
}

// where a byte of a line ends up after its line edit.
static u64
MapByte(u64 byte, const LineEdit *lineEdit)
{
  if (byte <= lineEdit->at) return byte;
  if (byte < lineEdit->at + lineEdit->removed) return lineEdit->at;
  return byte - lineEdit->removed + lineEdit->length;
}

// rewrites one block of lines with a single replace, and records each line's edit.
static void
RewriteBlock(const EditContext *edit, LineBlock block, LineEditor editor, LineEdit *edits)
{
  Buffer *buffer = edit->view->buffer;
  StrBuf text = {};
  bool changed = false;

  for (u64 line = block.first; line <= block.last; line++) {
    const StrBuf *original = BufferLine(buffer, line);
    LineEdit lineEdit = editor(edit, original);
    edits[line - block.first] = lineEdit;
    changed = changed || lineEdit.removed || lineEdit.length;

    u64 tail = lineEdit.at + lineEdit.removed;
    StrBufAppend(&text, original->data, lineEdit.at);
    StrBufAppend(&text, lineEdit.text, lineEdit.length);
    StrBufAppend(&text, original->data + tail, original->length - tail);
    if (line < block.last) StrBufAppendByte(&text, '\n');
  }

  Position end = {block.last, BufferLine(buffer, block.last)->length};
  Range range = {{block.first, 0}, end};
  if (changed) BufferReplace(buffer, range, text.data, text.length);
  StrBufFree(&text);
}

// applies editor to every line touched by a cursor, as one replace per run of lines.
// the cursors are then put back on the same text, which the replaces alone would not do.
static void
EditLines(const EditContext *edit, LineEditor editor)
{
  CursorSet *set = &edit->view->cursors;
  CursorSetMerge(set);

  CursorSet before = {};
  CursorSetCopy(&before, set);
  u32 blockCount;
  LineBlock *blocks = TouchedBlocks(set, &blockCount);
  Begin(edit, UndoKindOther);

  for (u32 index = 0; index < blockCount; index++) {
    LineBlock block = blocks[index];
    LineEdit *edits = MemoryAllocate((block.last - block.first + 1) * sizeof(LineEdit));
    RewriteBlock(edit, block, editor, edits);

    // line edits never add or remove lines, so cursor lines stay as they were.
    for (u32 cursor = 0; cursor < set->count; cursor++) {
      Cursor *now = &set->cursors[cursor];
      Cursor *was = &before.cursors[cursor];
      Position *ends[2] = {&now->anchor, &now->head};
      Position originals[2] = {was->anchor, was->head};

      for (u32 end = 0; end < 2; end++) {
        Position original = originals[end];
        if (original.line < block.first || original.line > block.last) continue;

        LineEdit *lineEdit = &edits[original.line - block.first];
        *ends[end] = (Position){original.line, MapByte(original.byte, lineEdit)};
      }
    }

    free(edits);
  }

  End(edit);
  free(blocks);
  CursorSetFree(&before);
}

static LineEdit
IndentLine(const EditContext *edit, const StrBuf *line)
{
  if (line->length == 0) return (LineEdit){};

  LineEdit lineEdit = {};
  lineEdit.text = IndentUnit(edit, &lineEdit.length);
  return lineEdit;
}

static LineEdit
DedentLine([[maybe_unused]] const EditContext *edit, const StrBuf *line)
{
  if (line->length && line->data[0] == '\t') return (LineEdit){.removed = 1};

  u64 leading = 0;
  while (leading < line->length && line->data[leading] == ' ') leading++;

  // back to the previous tab stop.
  u64 removed = leading % (u64)tabWidth ? leading % (u64)tabWidth : (u64)tabWidth;
  return (LineEdit){.removed = removed < leading ? removed : leading};
}

static bool
AnySelection(const CursorSet *set)
{
  for (u32 index = 0; index < set->count; index++) {
    if (CursorHasSelection(&set->cursors[index])) return true;
  }

  return false;
}

void
EditIndent(const EditContext *edit)
{
  CursorSet *set = &edit->view->cursors;

  if (AnySelection(set)) {
    EditLines(edit, IndentLine);
    return;
  }

  Begin(edit, UndoKindTyping);

  for (u32 index = 0; index < set->count; index++) {
    Position head = set->cursors[index].head;

    u64 length = 1;
    const char *text = "\t";

    // spaces reach the next tab stop.
    if (edit->expandTab) {
      const StrBuf *line = BufferLine(edit->view->buffer, head.line);
      u64 column = LayoutColumn(line->data, line->length, head.byte);
      length = (u64)tabWidth - column % (u64)tabWidth;
      text = spaces;
    }

    ReplaceAt(edit, index, (Range){head, head}, text, length);
  }

  End(edit);
}

void
EditDedent(const EditContext *edit)
{
  EditLines(edit, DedentLine);
}

static bool
OnlySpacesBefore(const StrBuf *line, u64 byte)
{
  for (u64 index = 0; index < byte; index++) {
    if (line->data[index] != ' ') return false;
  }

  return true;
}

static Range
BackwardRange(const EditContext *edit, const Cursor *cursor)
{
  if (CursorHasSelection(cursor)) return CursorRange(cursor);

  Position head = cursor->head;
  const StrBuf *line = BufferLine(edit->view->buffer, head.line);

  // spaces standing in for a tab are deleted back to the previous tab stop.
  if (edit->expandTab && head.byte > 0 && OnlySpacesBefore(line, head.byte)) {
    u64 count = head.byte % (u64)tabWidth ? head.byte % (u64)tabWidth : (u64)tabWidth;
    return (Range){{head.line, head.byte - count}, head};
  }

  return (Range){MovePosition(edit->view, cursor, MoveLeft), head};
}

static Range
ForwardRange(const EditContext *edit, const Cursor *cursor)
{
  if (CursorHasSelection(cursor)) return CursorRange(cursor);
  return (Range){cursor->head, MovePosition(edit->view, cursor, MoveRight)};
}

static Range
WordBackwardRange(const EditContext *edit, const Cursor *cursor)
{
  if (CursorHasSelection(cursor)) return CursorRange(cursor);
  return (Range){MovePosition(edit->view, cursor, MoveWordLeft), cursor->head};
}

void
EditDeleteBackward(const EditContext *edit)
{
  ReplaceEach(edit, UndoKindDeleteBackward, BackwardRange);
}

void
EditDeleteForward(const EditContext *edit)
{
  ReplaceEach(edit, UndoKindDeleteForward, ForwardRange);
}

void
EditDeleteWordBackward(const EditContext *edit)
{
  ReplaceEach(edit, UndoKindOther, WordBackwardRange);
}

// appends lines first to last joined with "\n".
static void
AppendLines(const Buffer *buffer, LineBlock block, StrBuf *text)
{
  Position end = {block.last, BufferLine(buffer, block.last)->length};
  BufferGetText(buffer, (Range){{block.first, 0}, end}, text);
}

// the range that removes whole lines, including one line break.
static Range
LinesRange(const Buffer *buffer, LineBlock block)
{
  u64 lineCount = BufferLineCount(buffer);
  if (block.last + 1 < lineCount) return (Range){{block.first, 0}, {block.last + 1, 0}};
  if (block.first == 0) return (Range){{0, 0}, BufferEnd(buffer)};

  // the last lines take the line break before them instead.
  Position start = {block.first - 1, BufferLine(buffer, block.first - 1)->length};
  return (Range){start, BufferEnd(buffer)};
}

// moves both ends of a cursor by a number of lines, keeping their bytes.
// a selection ending at the start of the line after the moved lines has no such line once
// they are last, and ends at the end of the buffer instead.
static void
ShiftCursor(const Buffer *buffer, Cursor *cursor, const Cursor *original, i64 lines)
{
  Position anchor = original->anchor;
  Position head = original->head;
  anchor.line = (u64)((i64)anchor.line + lines);
  head.line = (u64)((i64)head.line + lines);
  cursor->anchor = BufferClamp(buffer, anchor);
  cursor->head = BufferClamp(buffer, head);
}

void
EditDuplicateLines(const EditContext *edit)
{
  View *view = edit->view;
  CursorSetCollapse(&view->cursors);
  Cursor *cursor = CursorSetPrimary(&view->cursors);
  Cursor original = *cursor;
  LineBlock block = TouchedLines(cursor);

  StrBuf text = {};
  StrBufAppendByte(&text, '\n');
  AppendLines(view->buffer, block, &text);
  Position end = {block.last, BufferLine(view->buffer, block.last)->length};

  Begin(edit, UndoKindOther);
  BufferReplace(view->buffer, (Range){end, end}, text.data, text.length);
  ShiftCursor(view->buffer, cursor, &original, (i64)(block.last - block.first + 1));
  End(edit);
  StrBufFree(&text);
}

// puts a cursor on a line at its desired column.
static void
PlaceOnLine(View *view, Cursor *cursor, u64 line)
{
  u64 lastLine = BufferLineCount(view->buffer) - 1;
  ViewRow row = {line < lastLine ? line : lastLine, 0};
  CursorPlace(cursor, ViewPositionAt(view, row, cursor->desiredColumn));
}

void
EditDeleteLines(const EditContext *edit)
{
  View *view = edit->view;
  CursorSetCollapse(&view->cursors);
  Cursor *cursor = CursorSetPrimary(&view->cursors);
  LineBlock block = TouchedLines(cursor);
  u64 desiredColumn = cursor->desiredColumn;

  Begin(edit, UndoKindOther);
  BufferReplace(view->buffer, LinesRange(view->buffer, block), "", 0);
  PlaceOnLine(view, cursor, block.first);
  End(edit);

  // repeated deletes keep aiming for the same column.
  cursor->desiredColumn = desiredColumn;
}

bool
EditMoveLines(const EditContext *edit, bool up)
{
  View *view = edit->view;
  Buffer *buffer = view->buffer;
  CursorSetCollapse(&view->cursors);
  Cursor *cursor = CursorSetPrimary(&view->cursors);
  LineBlock block = TouchedLines(cursor);

  if (up && block.first == 0) return false;
  if (!up && block.last + 1 >= BufferLineCount(buffer)) return false;

  // the lines swap places with their neighbour in one replace.
  u64 neighbour = up ? block.first - 1 : block.last + 1;
  LineBlock both = {up ? neighbour : block.first, up ? block.last : neighbour};
  StrBuf text = {};

  if (up) {
    AppendLines(buffer, block, &text);
    StrBufAppendByte(&text, '\n');
    AppendLines(buffer, (LineBlock){neighbour, neighbour}, &text);
  } else {
    AppendLines(buffer, (LineBlock){neighbour, neighbour}, &text);
    StrBufAppendByte(&text, '\n');
    AppendLines(buffer, block, &text);
  }

  Cursor original = *cursor;
  Position end = {both.last, BufferLine(buffer, both.last)->length};

  Begin(edit, UndoKindOther);
  BufferReplace(buffer, (Range){{both.first, 0}, end}, text.data, text.length);
  ShiftCursor(buffer, cursor, &original, up ? -1 : 1);
  End(edit);
  StrBufFree(&text);
  return true;
}

bool
EditCopy(View *view, StrBuf *text)
{
  CursorSet *set = &view->cursors;
  CursorSetMerge(set);
  bool lineWise = !AnySelection(set);
  u64 previousLine = UINT64_MAX;

  for (u32 index = 0; index < set->count; index++) {
    Cursor *cursor = &set->cursors[index];

    if (lineWise) {
      // several cursors on one line copy it once.
      if (cursor->head.line == previousLine) continue;

      previousLine = cursor->head.line;
      AppendLines(view->buffer, (LineBlock){previousLine, previousLine}, text);
      StrBufAppendByte(text, '\n');
    } else if (CursorHasSelection(cursor)) {
      if (text->length) StrBufAppendByte(text, '\n');
      BufferGetText(view->buffer, CursorRange(cursor), text);
    }
  }

  return lineWise;
}

static Range
SelectionRange([[maybe_unused]] const EditContext *edit, const Cursor *cursor)
{
  return CursorRange(cursor);
}

// deletes the line of each cursor, from the bottom up so the lines above stay put.
static void
CutLines(const EditContext *edit)
{
  View *view = edit->view;
  CursorSet *set = &view->cursors;
  u64 previousLine = UINT64_MAX;
  Begin(edit, UndoKindOther);

  for (u32 index = set->count; index-- > 0;) {
    Cursor *cursor = &set->cursors[index];
    u64 line = cursor->head.line;
    if (line == previousLine) continue;

    previousLine = line;
    BufferReplace(view->buffer, LinesRange(view->buffer, (LineBlock){line, line}), "", 0);
  }

  for (u32 index = 0; index < set->count; index++) {
    Cursor *cursor = &set->cursors[index];
    PlaceOnLine(view, cursor, cursor->head.line);
  }

  End(edit);
}

bool
EditCut(const EditContext *edit, StrBuf *text)
{
  bool lineWise = EditCopy(edit->view, text);
  if (lineWise) CutLines(edit);
  else ReplaceEach(edit, UndoKindOther, SelectionRange);
  return lineWise;
}

u64
EditTrimTrailingWhitespace(const EditContext *edit)
{
  Buffer *buffer = edit->view->buffer;
  u64 trimmed = 0;
  Begin(edit, UndoKindOther);

  for (u64 line = 0; line < BufferLineCount(buffer); line++) {
    const StrBuf *text = BufferLine(buffer, line);
    u64 end = text->length;
    while (end > 0 && IsBlank(text->data[end - 1])) end--;
    if (end == text->length) continue;

    BufferReplace(buffer, (Range){{line, end}, {line, text->length}}, "", 0);
    trimmed++;
  }

  End(edit);
  return trimmed;
}

bool
EditUndo(const EditContext *edit)
{
  if (!UndoUndo(edit->undo, &edit->view->cursors)) return false;

  ResetDesiredColumns(edit->view);
  return true;
}

bool
EditRedo(const EditContext *edit)
{
  if (!UndoRedo(edit->undo, &edit->view->cursors)) return false;

  ResetDesiredColumns(edit->view);
  return true;
}
