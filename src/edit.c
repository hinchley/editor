#include "edit.h"

#include <stdlib.h>
#include <string.h>

#include "layout.h"
#include "memory.h"
#include "move.h"
#include "settings.h"
#include "utf8.h"

// the change one line gets from a line command: removed bytes at an offset replaced by
// text.
typedef struct LineEdit {
  u64 at;
  u64 removed;
  const char *text;
  u64 length;
} LineEdit;

// a run of lines, first to last inclusive.
typedef struct LineBlock {
  u64 first;
  u64 last;
} LineBlock;

// what a line editor works from besides the line: the command, the block of touched
// lines the line is in, and the editor's own settings.
typedef struct LineTask {
  const EditContext *edit;
  LineBlock block;
  void *context;
} LineTask;

typedef LineEdit (*LineEditor)(const LineTask *task, const StrBuf *line);

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
static_assert((u64)tabWidth < sizeof(spaces));

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

static char
CloserOf(char opener)
{
  switch (opener) {
    case '(':
      return ')';
    case '[':
      return ']';
    case '{':
      return '}';
    default:
      return '\0';
  }
}

// the closer that typed text wraps a selection with, when the text is an opening bracket
// or a quote, or 0.
static char
SurroundCloser(const char *text, u64 length)
{
  if (length != 1 || !text[0]) return '\0';
  if (strchr("\"'`", text[0])) return text[0];
  return CloserOf(text[0]);
}

// puts opener and closer around the selection of cursor index, which stays on the text
// inside, facing the same way.
static void
SurroundAt(const EditContext *edit, u32 index, char opener, char closer)
{
  Buffer *buffer = edit->view->buffer;
  Cursor *cursor = &edit->view->cursors.cursors[index];
  Range range = CursorRange(cursor);
  bool forward = PositionCompare(cursor->anchor, cursor->head) <= 0;

  BufferReplace(buffer, (Range){range.end, range.end}, &closer, 1);
  Position start = BufferReplace(buffer, (Range){range.start, range.start}, &opener, 1);
  Position end = range.end;
  if (end.line == range.start.line) end.byte++;

  cursor->anchor = forward ? start : end;
  cursor->head = forward ? end : start;
}

void
EditInsert(const EditContext *edit, const char *text, u64 length)
{
  CursorSet *set = &edit->view->cursors;
  char closer = SurroundCloser(text, length);
  bool surround = closer && CursorSetHasSelection(set);

  // a bracket or quote typed over a selection wraps it, as a step of its own.
  Begin(edit, surround ? UndoKindOther : UndoKindTyping);

  for (u32 index = 0; index < set->count; index++) {
    Cursor *cursor = &set->cursors[index];
    if (surround && CursorHasSelection(cursor)) SurroundAt(edit, index, text[0], closer);
    else ReplaceAt(edit, index, CursorRange(cursor), text, length);
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

// the number of lines in text, not counting the empty one after the final line break of
// line wise text.
static u32
CountPieces(const char *text, u64 length, bool lineWise)
{
  u64 count = 1;

  for (u64 index = 0; index < length; index++) count += text[index] == '\n';
  if (lineWise && length && text[length - 1] == '\n') count--;
  return count > UINT32_MAX ? UINT32_MAX : (u32)count;
}

void
EditPaste(const EditContext *edit, const char *text, u64 length, bool lineWise)
{
  StrBuf normal = {};
  AppendNormalised(&normal, text, length);
  const char *data = normal.data ? normal.data : "";

  CursorSet *set = &edit->view->cursors;
  CursorSetMerge(set);
  u32 pieceCount = set->count > 1 ? CountPieces(data, normal.length, lineWise) : 0;
  bool distribute = pieceCount == set->count;
  const char *piece = data;
  Begin(edit, UndoKindOther);

  for (u32 index = 0; index < set->count; index++) {
    Cursor *cursor = &set->cursors[index];
    const char *pieceText = data;
    u64 pieceLength = normal.length;

    // one line per cursor, in order, keeping a line wise piece's line break.
    if (distribute) {
      const char *end = data + normal.length;
      const char *lineBreak = memchr(piece, '\n', (u64)(end - piece));
      const char *pieceEnd = lineBreak ? lineBreak : end;
      pieceText = piece;
      pieceLength = (u64)(pieceEnd - piece) + (lineWise && lineBreak);
      piece = lineBreak ? lineBreak + 1 : end;
    }

    // whole lines go in above, and the cursor moves down with its own line.
    if (lineWise && !CursorHasSelection(cursor)) {
      Range lineStart = {{cursor->head.line, 0}, {cursor->head.line, 0}};
      BufferReplace(edit->view->buffer, lineStart, pieceText, pieceLength);
      continue;
    }

    ReplaceAt(edit, index, CursorRange(cursor), pieceText, pieceLength);
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

void
EditOpenLine(const EditContext *edit, bool above)
{
  Buffer *buffer = edit->view->buffer;
  CursorSet *set = &edit->view->cursors;
  Begin(edit, UndoKindOther);

  for (u32 index = 0; index < set->count; index++) {
    Cursor *cursor = &set->cursors[index];
    u64 lineIndex = cursor->head.line;
    const StrBuf *line = BufferLine(buffer, lineIndex);

    if (!above) {
      CursorPlace(cursor, (Position){lineIndex, line->length});
      NewlineAt(edit, index);
      continue;
    }

    u64 indent = IndentLength(line);
    StrBuf text = {};
    StrBufAppend(&text, line->data, indent);
    StrBufAppendByte(&text, '\n');
    Position start = {lineIndex, 0};
    BufferReplace(buffer, (Range){start, start}, text.data, text.length);
    CursorPlace(cursor, (Position){lineIndex, indent});
    StrBufFree(&text);
  }

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

// edits each line of a block with a replace of just the bytes that change, so every view
// of the buffer keeps its cursors on the same text, and records each line's edit.
static void
RewriteBlock(const LineTask *task, LineEditor editor, LineEdit *edits)
{
  Buffer *buffer = task->edit->view->buffer;
  LineBlock block = task->block;

  for (u64 line = block.first; line <= block.last; line++) {
    LineEdit lineEdit = editor(task, BufferLine(buffer, line));
    edits[line - block.first] = lineEdit;
    if (!lineEdit.removed && !lineEdit.length) continue;

    Range range = {{line, lineEdit.at}, {line, lineEdit.at + lineEdit.removed}};
    BufferReplace(buffer, range, lineEdit.text ? lineEdit.text : "", lineEdit.length);
  }
}

// applies editor to every line touched by a cursor.
// the cursors are then put back as MapByte says, which keeps a cursor at the start of an
// indented line before the new indentation.
static void
EditLines(const EditContext *edit, LineEditor editor, void *context)
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
    RewriteBlock(&(LineTask){edit, block, context}, editor, edits);

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
IndentLine(const LineTask *task, const StrBuf *line)
{
  if (line->length == 0) return (LineEdit){};

  LineEdit lineEdit = {};
  lineEdit.text = IndentUnit(task->edit, &lineEdit.length);
  return lineEdit;
}

static LineEdit
DedentLine([[maybe_unused]] const LineTask *task, const StrBuf *line)
{
  if (line->length && line->data[0] == '\t') return (LineEdit){.removed = 1};

  u64 leading = 0;
  while (leading < line->length && line->data[leading] == ' ') leading++;

  // back to the previous tab stop.
  u64 removed = leading % (u64)tabWidth ? leading % (u64)tabWidth : (u64)tabWidth;
  return (LineEdit){.removed = removed < leading ? removed : leading};
}

void
EditIndent(const EditContext *edit)
{
  CursorSet *set = &edit->view->cursors;

  if (CursorSetHasSelection(set)) {
    EditLines(edit, IndentLine, nullptr);
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
  EditLines(edit, DedentLine, nullptr);
}

// how a comment toggle goes: the comment's text, what to do, and the indentation of the
// block last measured.
typedef struct Commenting {
  const char *start;
  u64 startLength;
  const char *end;
  u64 endLength;

  // remove comments rather than add them, and with only blank lines, comment those too.
  bool remove;
  bool blankLines;

  // the pass that puts or removes the end of a block comment.
  bool closing;

  LineBlock measured;
  u64 indent;
} Commenting;

static bool
IsBlankLine(const StrBuf *line)
{
  return IndentLength(line) == line->length;
}

// the length of a line without its trailing blanks.
static u64
TrimmedLength(const StrBuf *line)
{
  u64 length = line->length;
  while (length > 0 && IsBlank(line->data[length - 1])) length--;
  return length;
}

static bool
HasAt(const StrBuf *line, u64 at, const char *text, u64 length)
{
  return at + length <= line->length && memcmp(line->data + at, text, length) == 0;
}

static bool
IsCommented(const Commenting *comment, const StrBuf *line)
{
  u64 indent = IndentLength(line);
  u64 trimmed = TrimmedLength(line);
  if (!HasAt(line, indent, comment->start, comment->startLength)) return false;
  if (!comment->endLength) return true;

  u64 room = comment->startLength + comment->endLength;
  if (trimmed < indent + room) return false;
  return HasAt(line, trimmed - comment->endLength, comment->end, comment->endLength);
}

// the smallest indentation of the lines in a block that are not blank, so the comment
// markers line up.
static u64
BlockIndent(const LineTask *task, Commenting *comment)
{
  LineBlock block = task->block;
  bool known = comment->measured.first == block.first && comment->measured.last == block.last;
  if (known) return comment->indent;

  const Buffer *buffer = task->edit->view->buffer;
  u64 indent = UINT64_MAX;

  for (u64 line = block.first; line <= block.last; line++) {
    const StrBuf *text = BufferLine(buffer, line);
    u64 length = IndentLength(text);
    if (length < text->length && length < indent) indent = length;
  }

  comment->measured = block;
  comment->indent = indent;
  return indent;
}

static LineEdit
OpenComment(const LineTask *task, Commenting *comment, const StrBuf *line)
{
  if (comment->remove) {
    u64 at = IndentLength(line);
    u64 removed = comment->startLength;
    if (HasAt(line, at + removed, " ", 1)) removed++;
    return (LineEdit){.at = at, .removed = removed};
  }

  u64 at = BlockIndent(task, comment);
  if (at > line->length) at = line->length;

  // the text is the start and a space, which follows it in the comment string.
  return (LineEdit){.at = at, .text = comment->start, .length = comment->startLength + 1};
}

static LineEdit
CloseComment(Commenting *comment, const StrBuf *line)
{
  if (!comment->remove) {
    // the text is a space and the end, which follow each other in the comment string.
    const char *text = comment->end - 1;
    return (LineEdit){.at = line->length, .text = text, .length = comment->endLength + 1};
  }

  u64 end = TrimmedLength(line);
  u64 at = end - comment->endLength;
  if (at > 0 && line->data[at - 1] == ' ') at--;
  return (LineEdit){.at = at, .removed = end - at};
}

static LineEdit
CommentLine(const LineTask *task, const StrBuf *line)
{
  Commenting *comment = task->context;
  if (!comment->blankLines && IsBlankLine(line)) return (LineEdit){};
  if (comment->closing) return CloseComment(comment, line);
  return OpenComment(task, comment, line);
}

// decides whether a toggle removes comments: only when every line it touches that is
// not blank has one.
static void
DecideComment(const EditContext *edit, Commenting *comment)
{
  CursorSet *set = &edit->view->cursors;
  CursorSetMerge(set);
  u32 blockCount;
  LineBlock *blocks = TouchedBlocks(set, &blockCount);
  bool anyText = false;
  bool allCommented = true;

  for (u32 index = 0; index < blockCount; index++) {
    for (u64 line = blocks[index].first; line <= blocks[index].last; line++) {
      const StrBuf *text = BufferLine(edit->view->buffer, line);
      if (IsBlankLine(text)) continue;

      anyText = true;
      if (!IsCommented(comment, text)) allCommented = false;
    }
  }

  free(blocks);
  comment->remove = anyText && allCommented;
  comment->blankLines = !anyText;
}

void
EditToggleComment(const EditContext *edit, const char *start, const char *end)
{
  // the markers are put in with a space between them and the text.
  StrBuf text = {};
  StrBufAppendFormat(&text, "%s %s", start, end);

  Commenting comment = {
    .start = text.data,
    .startLength = strlen(start),
    .end = text.data + strlen(start) + 1,
    .endLength = strlen(end),
    .measured = {UINT64_MAX, UINT64_MAX}
  };

  DecideComment(edit, &comment);
  Begin(edit, UndoKindOther);
  EditLines(edit, CommentLine, &comment);

  if (comment.endLength) {
    comment.closing = true;
    EditLines(edit, CommentLine, &comment);
  }

  End(edit);
  StrBufFree(&text);
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

static Range
WordForwardRange(const EditContext *edit, const Cursor *cursor)
{
  if (CursorHasSelection(cursor)) return CursorRange(cursor);
  return (Range){cursor->head, MovePosition(edit->view, cursor, MoveWordRight)};
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

void
EditDeleteWordForward(const EditContext *edit)
{
  ReplaceEach(edit, UndoKindOther, WordForwardRange);
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

  // the copy goes above, so cursors in any view move down with the lines they were on.
  StrBuf text = {};
  AppendLines(view->buffer, block, &text);
  StrBufAppendByte(&text, '\n');
  Position start = {block.first, 0};

  Begin(edit, UndoKindOther);
  BufferReplace(view->buffer, (Range){start, start}, text.data, text.length);
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

  // the neighbour line is taken out and put back on the other side, so the moved lines
  // are never replaced and cursors on them in any view go with them.
  u64 neighbour = up ? block.first - 1 : block.last + 1;
  StrBuf text = {};
  AppendLines(buffer, (LineBlock){neighbour, neighbour}, &text);
  Cursor original = *cursor;
  Begin(edit, UndoKindOther);

  if (up) {
    BufferReplace(buffer, (Range){{neighbour, 0}, {neighbour + 1, 0}}, "", 0);
    StrBufInsert(&text, 0, "\n", 1);
    Position end = {block.last - 1, BufferLine(buffer, block.last - 1)->length};
    BufferReplace(buffer, (Range){end, end}, text.data, text.length);
  } else {
    Position end = {block.last, BufferLine(buffer, block.last)->length};
    Position neighbourEnd = {neighbour, BufferLine(buffer, neighbour)->length};
    BufferReplace(buffer, (Range){end, neighbourEnd}, "", 0);
    StrBufAppendByte(&text, '\n');
    Position start = {block.first, 0};
    BufferReplace(buffer, (Range){start, start}, text.data, text.length);
  }

  ShiftCursor(buffer, cursor, &original, up ? -1 : 1);
  End(edit);
  StrBufFree(&text);
  return true;
}

// how a sort compares two lines.
// widths, when sorting by length, holds the display width of each line from first on.
typedef struct Sorting {
  const StrBuf *lines;
  const u64 *widths;
  u64 first;
  bool ignoreCase;
  bool descending;
} Sorting;

static i32
CompareBytes(const StrBuf *a, const StrBuf *b)
{
  u64 length = a->length < b->length ? a->length : b->length;
  i32 order = length ? memcmp(a->data, b->data, length) : 0;
  if (order) return order < 0 ? -1 : 1;
  return (a->length > b->length) - (a->length < b->length);
}

// compares the case folded code points, with ascii taken a byte at a time.
static i32
CompareFolded(const StrBuf *a, const StrBuf *b)
{
  // identical bytes need no folding, so folding starts after the last ascii byte of the
  // common prefix, which always ends a code point.
  u64 limit = a->length < b->length ? a->length : b->length;
  u64 boundary = 0;

  for (u64 same = 0; same < limit && a->data[same] == b->data[same]; same++) {
    if ((u8)a->data[same] < 0x80) boundary = same + 1;
  }

  u64 first = boundary;
  u64 second = boundary;

  while (first < a->length && second < b->length) {
    u32 x = (u8)a->data[first];
    u32 y = (u8)b->data[second];

    if (x < 0x80 && y < 0x80) {
      first++;
      second++;
      if (x >= 'A' && x <= 'Z') x += 32;
      if (y >= 'A' && y <= 'Z') y += 32;
    } else {
      first += Utf8Decode(a->data + first, a->length - first, &x);
      second += Utf8Decode(b->data + second, b->length - second, &y);
      x = Utf8Fold(x);
      y = Utf8Fold(y);
    }

    if (x != y) return x < y ? -1 : 1;
  }

  return (first < a->length) - (second < b->length);
}

// descending applies to the key: lines of equal width stay in ascending text order.
static i32
CompareLines(const Sorting *sorting, u64 a, u64 b)
{
  i32 direction = sorting->descending ? -1 : 1;

  if (sorting->widths) {
    u64 x = sorting->widths[a - sorting->first];
    u64 y = sorting->widths[b - sorting->first];
    if (x != y) return x < y ? -direction : direction;
    direction = 1;
  }

  const StrBuf *first = &sorting->lines[a];
  const StrBuf *second = &sorting->lines[b];
  i32 order = sorting->ignoreCase ? CompareFolded(first, second) : 0;
  return direction * (order ? order : CompareBytes(first, second));
}

// a stable merge sort of line indexes, with insertion sort for short runs.
static void
MergeSort(const Sorting *sorting, u64 *items, u64 *scratch, u64 count)
{
  if (count <= 16) {
    for (u64 index = 1; index < count; index++) {
      u64 item = items[index];
      u64 at = index;

      while (at > 0 && CompareLines(sorting, items[at - 1], item) > 0) {
        items[at] = items[at - 1];
        at--;
      }

      items[at] = item;
    }

    return;
  }

  u64 half = count / 2;
  MergeSort(sorting, items, scratch, half);
  MergeSort(sorting, items + half, scratch, count - half);
  if (CompareLines(sorting, items[half - 1], items[half]) <= 0) return;

  u64 left = 0;
  u64 right = half;
  u64 out = 0;

  while (left < half && right < count) {
    bool takeRight = CompareLines(sorting, items[right], items[left]) < 0;
    scratch[out++] = takeRight ? items[right++] : items[left++];
  }

  while (left < half) scratch[out++] = items[left++];
  memcpy(items, scratch, out * sizeof(u64));
}

// drops each line equal to the one kept before it and returns the new count.
// the case folded orders compare without the byte order tie break, so lines differing
// only in case are equal.
static u64
KeepUnique(const Sorting *sorting, u64 *items, u64 count)
{
  u64 kept = 0;

  for (u64 index = 0; index < count; index++) {
    const StrBuf *line = &sorting->lines[items[index]];
    const StrBuf *previous = kept ? &sorting->lines[items[kept - 1]] : nullptr;
    bool equal = false;

    if (previous) {
      i32 order = sorting->ignoreCase ? CompareFolded(previous, line) : 0;
      equal = !order && (sorting->ignoreCase || CompareBytes(previous, line) == 0);
    }

    if (!equal) items[kept++] = items[index];
  }

  return kept;
}

SortResult
EditSortLines(const EditContext *edit, SortOptions options)
{
  View *view = edit->view;
  Buffer *buffer = view->buffer;
  CursorSetCollapse(&view->cursors);
  Cursor *cursor = CursorSetPrimary(&view->cursors);
  LineBlock block = TouchedLines(cursor);
  u64 count = block.last - block.first + 1;

  // widths are measured once here, not in every comparison.
  u64 *widths = nullptr;

  if (options.key == SortKeyLength) {
    widths = MemoryAllocate(count * sizeof(u64));

    for (u64 index = 0; index < count; index++) {
      const StrBuf *line = BufferLine(buffer, block.first + index);
      widths[index] = LayoutColumn(line->data, line->length, line->length);
    }
  }

  Sorting sorting = {
    .lines = buffer->lines,
    .widths = widths,
    .first = block.first,
    .ignoreCase = options.ignoreCase,
    .descending = options.descending
  };
  u64 *items = MemoryAllocate(count * sizeof(u64));
  u64 *scratch = MemoryAllocate(count * sizeof(u64));
  for (u64 index = 0; index < count; index++) items[index] = block.first + index;
  MergeSort(&sorting, items, scratch, count);

  u64 kept = count;
  if (options.unique && options.key == SortKeyText) kept = KeepUnique(&sorting, items, count);

  StrBuf text = {};
  bool changed = kept != count;

  for (u64 index = 0; index < kept; index++) {
    const StrBuf *line = BufferLine(buffer, items[index]);
    if (index) StrBufAppendByte(&text, '\n');
    StrBufAppend(&text, line->data, line->length);
    changed = changed || items[index] != block.first + index;
  }

  Position start = {block.first, 0};
  Position end = {block.last, BufferLine(buffer, block.last)->length};
  Begin(edit, UndoKindOther);

  // lines already in order are left alone, which keeps the history clean.
  if (changed) {
    const char *data = text.data ? text.data : "";
    end = BufferReplace(buffer, (Range){start, end}, data, text.length);
  }

  cursor->anchor = start;
  cursor->head = end;
  End(edit);

  StrBufFree(&text);
  free(scratch);
  free(items);
  free(widths);
  return (SortResult){kept, count - kept};
}

// joins a line with the next one and returns where they meet.
static Position
JoinAt(Buffer *buffer, u64 line)
{
  const StrBuf *text = BufferLine(buffer, line);
  const StrBuf *next = BufferLine(buffer, line + 1);
  u64 indent = IndentLength(next);

  bool nextEmpty = indent == next->length;
  bool closer = !nextEmpty && next->data[indent] && strchr(")]}", next->data[indent]);
  bool endsBlank = !text->length || IsBlank(text->data[text->length - 1]);
  u64 space = !nextEmpty && !closer && !endsBlank;

  Position join = {line, text->length};
  BufferReplace(buffer, (Range){join, {line + 1, indent}}, " ", space);
  return join;
}

bool
EditJoinLines(const EditContext *edit)
{
  View *view = edit->view;
  Buffer *buffer = view->buffer;
  CursorSetCollapse(&view->cursors);
  Cursor *cursor = CursorSetPrimary(&view->cursors);
  LineBlock block = TouchedLines(cursor);
  if (block.last == block.first) block.last++;
  if (block.last >= BufferLineCount(buffer)) return false;

  // each join replaces only the line break and blanks, so other views keep their text.
  Begin(edit, UndoKindOther);
  Position join = {};
  for (u64 line = block.first; line < block.last; line++) join = JoinAt(buffer, block.first);
  CursorPlace(cursor, join);
  End(edit);
  return true;
}

bool
EditCopy(View *view, StrBuf *text)
{
  CursorSet *set = &view->cursors;
  CursorSetMerge(set);
  bool lineWise = !CursorSetHasSelection(set);
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

// the context counts the lines trimmed.
static LineEdit
TrimLine(const LineTask *task, const StrBuf *line)
{
  u64 end = line->length;
  while (end > 0 && IsBlank(line->data[end - 1])) end--;
  if (end == line->length) return (LineEdit){};

  (*(u64 *)task->context)++;
  return (LineEdit){.at = end, .removed = line->length - end};
}

u64
EditTrimLines(const EditContext *edit)
{
  u64 trimmed = 0;
  EditLines(edit, TrimLine, &trimmed);
  return trimmed;
}

// the number of code points before byte in line.
static u64
CodePointsBefore(const StrBuf *line, u64 byte)
{
  u64 count = 0;
  u64 index = 0;

  while (index < byte) {
    u32 codepoint;
    index += Utf8Decode(line->data + index, line->length - index, &codepoint);
    count++;
  }

  return count;
}

// the byte offset of code point number count in line.
static u64
ByteOfCodePoint(const StrBuf *line, u64 count)
{
  u64 index = 0;

  while (count-- > 0 && index < line->length) {
    u32 codepoint;
    index += Utf8Decode(line->data + index, line->length - index, &codepoint);
  }

  return index;
}

// converts the case of text and gives the edit as the bytes from the first code point that
// changes to the end of the last one, replaced by their converted form in output.
// code points without a mapping, invalid bytes among them, are copied unchanged.
static LineEdit
ChangeCaseOf(const char *text, u64 length, bool upper, StrBuf *output)
{
  LineEdit lineEdit = {};
  bool changed = false;
  u64 index = 0;
  output->length = 0;

  while (index < length) {
    u32 codepoint;
    u32 used = Utf8Decode(text + index, length - index, &codepoint);
    u32 mapped = upper ? Utf8Upper(codepoint) : Utf8Lower(codepoint);

    if (mapped != codepoint) {
      if (!changed) lineEdit.at = index;
      char encoded[4];
      StrBufAppend(output, encoded, Utf8Encode(mapped, encoded));
      changed = true;
      lineEdit.removed = index + used - lineEdit.at;
      lineEdit.length = output->length;
    } else if (changed) {
      StrBufAppend(output, text + index, used);
    }

    index += used;
  }

  lineEdit.text = output->data;
  return lineEdit;
}

// where a cursor end was, as a line and a code point within it, which case conversion
// keeps even when byte lengths change.
typedef struct CodePointPosition {
  u64 line;
  u64 codepoint;
} CodePointPosition;

u64
EditChangeCase(const EditContext *edit, bool upper)
{
  View *view = edit->view;
  Buffer *buffer = view->buffer;
  CursorSet *set = &view->cursors;
  CursorSetMerge(set);

  CodePointPosition *ends = MemoryAllocate(2 * set->count * sizeof(CodePointPosition));

  for (u32 index = 0; index < set->count; index++) {
    Position positions[2] = {set->cursors[index].anchor, set->cursors[index].head};

    for (u32 end = 0; end < 2; end++) {
      const StrBuf *line = BufferLine(buffer, positions[end].line);
      u64 codepoint = CodePointsBefore(line, positions[end].byte);
      ends[2 * index + end] = (CodePointPosition){positions[end].line, codepoint};
    }
  }

  StrBuf output = {};
  u64 changed = 0;
  u64 counted = UINT64_MAX;
  Begin(edit, UndoKindOther);

  // from the last selection and line back, so the offsets still to convert stay put.
  for (u32 index = set->count; index-- > 0;) {
    Range range = CursorRange(&set->cursors[index]);

    for (u64 line = range.end.line + 1; line-- > range.start.line;) {
      const StrBuf *text = BufferLine(buffer, line);
      u64 from = line == range.start.line ? range.start.byte : 0;
      u64 to = line == range.end.line ? range.end.byte : text->length;
      LineEdit lineEdit = ChangeCaseOf(text->data + from, to - from, upper, &output);
      if (!lineEdit.removed) continue;

      u64 at = from + lineEdit.at;
      Range piece = {{line, at}, {line, at + lineEdit.removed}};
      BufferReplace(buffer, piece, lineEdit.text, lineEdit.length);
      if (line != counted) changed++;
      counted = line;
    }
  }

  for (u32 index = 0; index < set->count; index++) {
    Cursor *cursor = &set->cursors[index];
    Position *positions[2] = {&cursor->anchor, &cursor->head};

    for (u32 end = 0; end < 2; end++) {
      CodePointPosition was = ends[2 * index + end];
      u64 byte = ByteOfCodePoint(BufferLine(buffer, was.line), was.codepoint);
      *positions[end] = (Position){was.line, byte};
    }
  }

  End(edit);
  StrBufFree(&output);
  free(ends);
  return changed;
}

static Position
StartOf(const CursorSet *set, u32 index)
{
  return CursorRange(&set->cursors[index]).start;
}

u64
EditAlignCursors(const EditContext *edit)
{
  Buffer *buffer = edit->view->buffer;
  CursorSet *set = &edit->view->cursors;
  CursorSetMerge(set);

  // each cursor's place among the cursors on its line, which padding never changes, as it
  // only inserts spaces on the cursor's own line.
  // the cursors of a line are next to each other, so a line's first is index - ranks[index].
  u32 *ranks = MemoryAllocate(set->count * sizeof(u32));
  u64 *columns = MemoryAllocate(set->count * sizeof(u64));
  bool *padded = MemoryAllocate(set->count * sizeof(bool));
  u32 rankCount = 0;

  for (u32 index = 0; index < set->count; index++) {
    bool sameLine = index > 0 && StartOf(set, index).line == StartOf(set, index - 1).line;
    ranks[index] = sameLine ? ranks[index - 1] + 1 : 0;
    padded[index] = false;
    if (ranks[index] == rankCount) rankCount++;
  }

  StrBuf padding = {};
  u64 changed = 0;
  Begin(edit, UndoKindOther);

  // rank by rank from the left, so padding an earlier rank moves the later ones first.
  for (u32 rank = 0; rank < rankCount; rank++) {
    u64 target = 0;
    u32 lines = 0;

    for (u32 index = 0; index < set->count; index++) {
      if (ranks[index] != rank) continue;

      Position start = StartOf(set, index);
      const StrBuf *line = BufferLine(buffer, start.line);
      columns[index] = LayoutColumn(line->data, line->length, start.byte);
      if (columns[index] > target) target = columns[index];
      lines++;
    }

    // no rank is on more lines than the one before, so none after this one has two.
    if (lines < 2) break;

    for (u32 index = 0; index < set->count; index++) {
      if (ranks[index] != rank || columns[index] == target) continue;

      u64 missing = target - columns[index];
      while (padding.length < missing) StrBufAppend(&padding, spaces, sizeof(spaces) - 1);

      // the cursor is at the end of the insertion, so it moves along with its text.
      Position start = StartOf(set, index);
      BufferReplace(buffer, (Range){start, start}, padding.data, missing);

      u32 first = index - ranks[index];
      if (!padded[first]) changed++;
      padded[first] = true;
    }
  }

  End(edit);
  StrBufFree(&padding);
  free(padded);
  free(columns);
  free(ranks);
  return changed;
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
