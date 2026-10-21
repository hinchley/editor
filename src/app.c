#include "app.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "clipboard.h"
#include "document.h"
#include "edit.h"
#include "input.h"
#include "issues.h"
#include "jump.h"
#include "keymap.h"
#include "lang.h"
#include "launch.h"
#include "learn.h"
#include "lookup.h"
#include "lsp.h"
#include "markdown.h"
#include "memory.h"
#include "mouse.h"
#include "move.h"
#include "multicursor.h"
#include "multifile.h"
#include "picker.h"
#include "places.h"
#include "preview.h"
#include "project.h"
#include "prompt.h"
#include "refactor.h"
#include "render.h"
#include "review.h"
#include "save.h"
#include "scan.h"
#include "screen.h"
#include "search.h"
#include "session.h"
#include "settings.h"
#include "symbols.h"
#include "term.h"
#include "utf8.h"
#include "view.h"
#include "workspace.h"

// how long an incomplete escape sequence may wait for the rest of its bytes.
static constexpr i32 escapeTimeout = 50;

static TermSize windowSize;

static Workspace workspace;

static DisplayOptions displayOptions = {
  .softWrap = softWrapDefault,
  .lineNumbers = lineNumbersDefault,
  .showWhitespace = showWhitespaceDefault,
  .ruler = rulerDefault,
  .rulerColumn = rulerColumnDefault
};

static bool expandTab = expandTabDefault;

// the widest a markdown table column grows, as last asked for.
static u64 tableColumnWidth = tableColumnWidthDefault;

// the message bar text, cleared by the next key.
static StrBuf message;

// the index, among the last frame's panes, of the focused one.
static u32 drawnFocus;

static bool running = true;

// the current document and its focused view.
// fetched afresh by every command, since opening a folder frees every document.
static Document *
Current(void)
{
  return WorkspaceDocument(&workspace);
}

static View *
CurrentView(void)
{
  return Current()->view;
}

static EditContext
Editing(void)
{
  Document *document = Current();
  return (EditContext){
    .view = document->view,
    .undo = &document->undo,
    .expandTab = expandTab
  };
}

static void
Toggle(bool *setting, const char *name)
{
  *setting = !*setting;
  StrBufAppendFormat(&message, "%s %s", name, *setting ? "on" : "off");
}

// starts or stops following every open document with its language server.
static void
RefreshServers(void)
{
  for (u64 index = 0; index < workspace.count; index++) {
    Document *document = workspace.entries[index].document;
    if (document) DocumentRefreshServer(document);
  }
}

// turns language servers off or on, or, when one has stopped while they are on, starts
// them all again.
static void
ToggleServers(void)
{
  bool restart = LspEnabled() && LspAnyStopped();
  bool on = restart || !LspEnabled();
  LspSetEnabled(false);
  RefreshServers();

  if (on) {
    LspSetEnabled(true);
    RefreshServers();
  }

  StrBufAppendString(
    &message,
    restart ? "language servers restarted"
      : on  ? "language servers on"
            : "language servers off"
  );
}

static void
Scroll(i64 count)
{
  if (!ViewScroll(CurrentView(), count)) StrBufAppendString(&message, "cannot scroll further");
}

static void
Quit(void)
{
  running = false;
}

static void
Reload(void)
{
  Document *document = Current();
  if (!DocumentReload(document, &message)) return;

  StrBufAppendFormat(&message, "reloaded %lu lines", BufferLineCount(&document->buffer));
}

static void
ReloadAnswered([[maybe_unused]] void *context, const char *answer, [[maybe_unused]] u64 length)
{
  if (answer && *answer == 'y') Reload();
}

static void
AskToReload(void)
{
  Document *document = Current();

  if (!document->file.path.length) {
    StrBufAppendString(&message, "the buffer has no file to reload");
    return;
  }

  if (!DocumentModified(document)) {
    Reload();
    return;
  }

  PromptChoose("discard unsaved changes and reload? (y/n)", "yn", ReloadAnswered, nullptr);
}

// reads a whole decimal number from text, which must be in [minimum, maximum].
static bool
ParseNumber(const char *text, u64 length, u64 minimum, u64 maximum, u64 *number)
{
  if (!length) return false;

  *number = 0;

  for (u64 index = 0; index < length; index++) {
    if (text[index] < '0' || text[index] > '9') return false;

    *number = *number * 10 + (u64)(text[index] - '0');
    if (*number > maximum) return false;
  }

  return *number >= minimum;
}

static void
GoToAnswered([[maybe_unused]] void *context, const char *answer, u64 length)
{
  if (!answer) return;

  // "line" or "line:column", both counted from 1.
  u64 lineLength = 0;
  while (lineLength < length && answer[lineLength] != ':') lineLength++;

  u64 line;
  u64 column = 1;
  bool valid = ParseNumber(answer, lineLength, 1, UINT32_MAX, &line);

  if (valid && lineLength < length) {
    const char *columnText = answer + lineLength + 1;
    valid = ParseNumber(columnText, length - lineLength - 1, 1, UINT32_MAX, &column);
  }

  if (!valid) {
    StrBufAppendFormat(&message, "not a line or line:column: %s", answer);
    return;
  }

  JumpRecord();
  MoveGoTo(CurrentView(), line - 1, column - 1);
}

static void
RulerAnswered([[maybe_unused]] void *context, const char *answer, u64 length)
{
  if (!answer) return;

  u64 column;
  u64 minimum = rulerColumnMinimum;
  u64 maximum = rulerColumnMaximum;

  if (!ParseNumber(answer, length, minimum, maximum, &column)) {
    StrBufAppendFormat(&message, "the ruler column must be from %lu to %lu", minimum, maximum);
    return;
  }

  displayOptions.rulerColumn = (i32)column;
  displayOptions.ruler = true;
  StrBufAppendFormat(&message, "ruler at column %lu", column);
}

static void
AskRulerColumn(void)
{
  StrBuf current = {};
  StrBufAppendFormat(&current, "%d", displayOptions.rulerColumn);
  PromptAsk("ruler column (1-500): ", current.data, RulerAnswered, nullptr);
  StrBufFree(&current);
}

static void
ToggleLineEndings(void)
{
  File *file = &Current()->file;
  file->lineEnding = file->lineEnding == LineEndingLf ? LineEndingCrlf : LineEndingLf;
  StrBufAppendFormat(&message, "line endings %s", FileLineEndingName(file->lineEnding));
}

// tells how much was copied or cut: whole lines, or characters within one line.
static void
DescribeCopy(const char *verb, const StrBuf *text, bool lineWise, bool system)
{
  u64 lines = 0;
  u64 characters = 0;

  for (u64 index = 0; index < text->length; index++) {
    lines += text->data[index] == '\n';
    characters += ((u8)text->data[index] & 0xC0) != 0x80;
  }

  // a line wise copy ends every line with a line break, and a selection does not.
  if (!lineWise && lines) lines++;
  u64 count = lines ? lines : characters;
  const char *unit = lines ? "line" : "character";

  StrBufAppendFormat(&message, "%s %lu %s%s", verb, count, unit, count == 1 ? "" : "s");
  if (!system) StrBufAppendString(&message, " (the system clipboard is not available)");
}

static void
Copy(bool cut)
{
  EditContext edit = Editing();
  StrBuf text = {};
  bool lineWise = cut ? EditCut(&edit, &text) : EditCopy(edit.view, &text);
  bool system = ClipboardSet(text.data ? text.data : "", text.length, lineWise);
  DescribeCopy(cut ? "cut" : "copied", &text, lineWise, system);
  StrBufFree(&text);
}

static void
Paste(void)
{
  StrBuf text = {};
  bool lineWise;
  ClipboardGet(&text, &lineWise);

  if (text.length) {
    EditContext edit = Editing();
    EditPaste(&edit, text.data, text.length, lineWise);
  } else {
    StrBufAppendString(&message, "the clipboard is empty");
  }

  StrBufFree(&text);
}

// escape collapses several cursors to one, then clears the selection, then stops
// highlighting matches.
static void
Cancel(void)
{
  CursorSet *set = &CurrentView()->cursors;

  if (set->count > 1) {
    CursorSetCollapse(set);
    return;
  }

  Cursor *cursor = CursorSetPrimary(set);

  if (CursorHasSelection(cursor)) {
    cursor->anchor = cursor->head;
    return;
  }

  SearchClear();
}

// comments out the touched lines in the language of the file, or uncomments them.
static void
ToggleComment(void)
{
  const Lang *lang = Current()->lang;

  if (!lang) {
    StrBufAppendString(&message, "no comments for this file type");
    return;
  }

  if (!lang->commentStart) {
    StrBufAppendFormat(&message, "%s has no comments", lang->name);
    return;
  }

  EditContext edit = Editing();
  EditToggleComment(&edit, lang->commentStart, lang->commentEnd);
}

static void
SortAnswered([[maybe_unused]] void *context, const char *answer, [[maybe_unused]] u64 length)
{
  if (!answer) return;

  // a capital letter ignores case.
  char letter = *answer;
  bool ignoreCase = letter >= 'A' && letter <= 'Z';
  if (ignoreCase) letter += 32;

  SortOptions options = {.ignoreCase = ignoreCase};
  options.key = letter == 's' || letter == 'l' ? SortKeyLength : SortKeyText;
  options.descending = letter == 'd' || letter == 'l';
  options.unique = letter == 'u';

  EditContext edit = Editing();
  SortResult result = EditSortLines(&edit, options);
  const char *plural = result.lines == 1 ? "" : "s";
  StrBufAppendFormat(&message, "sorted %lu line%s", result.lines, plural);

  if (result.removed) {
    plural = result.removed == 1 ? "" : "s";
    StrBufAppendFormat(&message, ", removed %lu duplicate%s", result.removed, plural);
  }
}

static void
AskSortOrder(void)
{
  // the sort works on the newest cursor's selection, as it collapses to that cursor.
  if (!CursorHasSelection(CursorSetPrimary(&Current()->view->cursors))) {
    StrBufAppendString(&message, "no selection");
    return;
  }

  const char *question =
    "sort: (a)sc (d)esc (u)nique (s)hortest (l)ongest; capital ignores case";
  PromptChoose(question, "aAdDuUsSlL", SortAnswered, nullptr);
}

static void
TableWidthAnswered([[maybe_unused]] void *context, const char *answer, u64 length)
{
  if (!answer) return;

  u64 width;
  u64 minimum = tableColumnWidthMinimum;
  u64 maximum = tableColumnWidthMaximum;

  if (!ParseNumber(answer, length, minimum, maximum, &width)) {
    StrBufAppendFormat(&message, "the column width must be from %lu to %lu", minimum, maximum);
    return;
  }

  tableColumnWidth = width;
  MarkdownTableResult result;

  if (!MarkdownAlignTable(Current(), width, &result)) {
    StrBufAppendString(&message, "not in a markdown table");
    return;
  }

  if (!result.changed) {
    StrBufAppendString(&message, "the table is aligned already");
    return;
  }

  const char *columns = result.columns == 1 ? "" : "s";
  const char *lines = result.lines == 1 ? "" : "s";
  StrBufAppendFormat(
    &message,
    "aligned %u column%s over %lu line%s",
    result.columns,
    columns,
    result.lines,
    lines
  );
}

static void
AskTableWidth(void)
{
  if (!MarkdownInTable(Current())) {
    StrBufAppendString(&message, "not in a markdown table");
    return;
  }

  StrBuf label = {};
  StrBuf current = {};
  StrBufAppendFormat(
    &label,
    "table column width (%lu-%lu): ",
    tableColumnWidthMinimum,
    tableColumnWidthMaximum
  );
  StrBufAppendFormat(&current, "%lu", tableColumnWidth);
  PromptAsk(label.data, current.data, TableWidthAnswered, nullptr);
  StrBufFree(&current);
  StrBufFree(&label);
}

static void
TrimWhitespace(bool everyLine)
{
  EditContext edit = Editing();
  u64 count = everyLine ? EditTrimTrailingWhitespace(&edit) : EditTrimLines(&edit);

  if (count) StrBufAppendFormat(&message, "trimmed %lu line%s", count, count == 1 ? "" : "s");
  else StrBufAppendString(&message, "no trailing whitespace");
}

static void
ChangeCase(bool upper)
{
  EditContext edit = Editing();

  if (!CursorSetHasSelection(&edit.view->cursors)) {
    StrBufAppendString(&message, "no selection");
    return;
  }

  u64 count = EditChangeCase(&edit, upper);
  const char *done = upper ? "uppercased" : "lowercased";

  if (count) StrBufAppendFormat(&message, "%s %lu line%s", done, count, count == 1 ? "" : "s");
  else StrBufAppendString(&message, "no change");
}

static void
AlignCursors(void)
{
  EditContext edit = Editing();
  CursorSet *set = &edit.view->cursors;
  CursorSetMerge(set);

  // merged cursors are in order, so the first and last are on different lines when any are.
  u64 first = CursorRange(&set->cursors[0]).start.line;
  u64 last = CursorRange(&set->cursors[set->count - 1]).start.line;

  if (first == last) {
    StrBufAppendString(&message, "align needs cursors on two or more lines");
    return;
  }

  u64 count = EditAlignCursors(&edit);

  if (count) StrBufAppendFormat(&message, "aligned %lu line%s", count, count == 1 ? "" : "s");
  else StrBufAppendString(&message, "already aligned");
}

static void
CopyPath(void)
{
  const StrBuf *path = &Current()->file.path;

  if (!path->length) {
    StrBufAppendString(&message, "the buffer has no file name");
    return;
  }

  bool system = ClipboardSet(path->data, path->length, false);
  StrBufAppendString(&message, "copied ");
  StrBufAppend(&message, path->data, path->length);
  if (!system) StrBufAppendString(&message, " (the system clipboard is not available)");
}

// opens a terminal in the folder of the buffer's file, or for an unnamed buffer in the
// folder the buffers were loaded from, or else the one the editor was started in.
static void
OpenTerminal(void)
{
  const StrBuf *path = &Current()->file.path;
  StrBuf folder = {};

  if (path->length) {
    const char *slash = strrchr(path->data, '/');
    if (!slash) StrBufAppendString(&folder, ".");
    else if (slash == path->data) StrBufAppendString(&folder, "/");
    else StrBufAppend(&folder, path->data, (u64)(slash - path->data));
  } else {
    const char *opened = SessionFolder();
    StrBufAppendString(&folder, opened ? opened : ".");
  }

  char real[PATH_MAX];

  if (realpath(folder.data, real)) LaunchTerminal(real, &message);
  else StrBufAppendFormat(&message, "the folder %s does not exist (save first)", folder.data);

  StrBufFree(&folder);
}

// what statistics counts.
typedef struct Counts {
  u64 lines;
  u64 words;
  u64 characters;
  u64 bytes;
} Counts;

// adds the words, characters and bytes of text within one line.
// words are runs of word characters, as word movement sees them, and characters are
// grapheme clusters.
static void
CountText(const char *text, u64 length, Counts *counts)
{
  bool inWord = false;

  for (u64 at = 0; at < length;) {
    u32 codepoint;
    at += Utf8Decode(text + at, length - at, &codepoint);
    bool word = Utf8IsWord(codepoint);
    counts->words += word && !inWord;
    inWord = word;
  }

  for (u64 at = 0; at < length; at = Utf8NextBoundary(text, length, at)) {
    counts->characters++;
  }

  counts->bytes += length;
}

// adds the text of range, where a line break is one character and as many bytes as the
// file's line ending.
static void
CountRange(const Buffer *buffer, Range range, u64 lineBreak, Counts *counts)
{
  for (u64 line = range.start.line; line <= range.end.line; line++) {
    const StrBuf *text = BufferLine(buffer, line);
    u64 start = line == range.start.line ? range.start.byte : 0;
    u64 end = line == range.end.line ? range.end.byte : text->length;
    CountText(text->data + start, end - start, counts);

    if (line < range.end.line) {
      counts->characters++;
      counts->bytes += lineBreak;
    }
  }
}

static void
AppendCounts(const Counts *part, const Counts *whole)
{
  const char *names[] = {"lines", "words", "characters", "bytes"};
  u64 wholeCounts[] = {whole->lines, whole->words, whole->characters, whole->bytes};
  u64 partCounts[] = {part->lines, part->words, part->characters, part->bytes};

  for (u32 index = 0; index < 4; index++) {
    if (index) StrBufAppendString(&message, ", ");
    if (part != whole) StrBufAppendFormat(&message, "%lu of ", partCounts[index]);
    StrBufAppendFormat(&message, "%lu %s", wholeCounts[index], names[index]);
  }
}

// reports the counts of the whole buffer, and of the selections when there are any.
static void
ShowStatistics(void)
{
  Document *document = Current();
  const Buffer *buffer = &document->buffer;
  u64 lineBreak = document->file.lineEnding == LineEndingCrlf ? 2 : 1;

  Counts whole = {.lines = BufferLineCount(buffer)};
  CountRange(buffer, (Range){{}, BufferEnd(buffer)}, lineBreak, &whole);

  CursorSet *set = &document->view->cursors;
  CursorSetMerge(set);
  Counts selected = {};
  bool anySelection = false;

  // the first line no selection has counted yet.
  u64 uncounted = 0;

  for (u32 index = 0; index < set->count; index++) {
    const Cursor *cursor = &set->cursors[index];
    if (!CursorHasSelection(cursor)) continue;

    // a selection ending at the start of a line does not count that line, and a line
    // shared by two selections counts once.
    Range range = CursorRange(cursor);
    u64 first = range.start.line > uncounted ? range.start.line : uncounted;
    u64 last = range.end.line;
    if (last > range.start.line && range.end.byte == 0) last--;

    if (last >= first) {
      selected.lines += last - first + 1;
      uncounted = last + 1;
    }

    CountRange(buffer, range, lineBreak, &selected);
    anySelection = true;
  }

  if (anySelection) StrBufAppendString(&message, "selection: ");
  AppendCounts(anySelection ? &selected : &whole, &whole);
}

static bool
MoveCommand(enum Command command)
{
  // the move commands, and the select commands after them, are listed in this order.
  static const enum Move moves[] = {
    MoveUp,
    MoveDown,
    MoveLeft,
    MoveRight,
    MoveLineStart,
    MoveLineEnd,
    MovePageUp,
    MovePageDown,
    MoveWordLeft,
    MoveWordRight,
    MoveFileStart,
    MoveFileEnd
  };

  if (command <= CommandMoveFileEnd) {
    // going to either end of the file is a jump, which alt+[ comes back from.
    if (command == CommandMoveFileStart || command == CommandMoveFileEnd) JumpRecord();
    MoveCursors(CurrentView(), moves[command - CommandMoveUp], false);
    return true;
  }

  if (command >= CommandSelectUp && command <= CommandSelectFileEnd) {
    MoveCursors(CurrentView(), moves[command - CommandSelectUp], true);
    return true;
  }

  return false;
}

static void
Report(bool done, const char *failure)
{
  if (!done) StrBufAppendString(&message, failure);
}

static bool
EditCommand(enum Command command)
{
  EditContext edit = Editing();

  switch (command) {
    case CommandNewline:
      EditNewline(&edit);
      break;
    case CommandIndent:
      EditIndent(&edit);
      break;
    case CommandDedent:
      EditDedent(&edit);
      break;
    case CommandDeleteBackward:
      EditDeleteBackward(&edit);
      break;
    case CommandDeleteForward:
      EditDeleteForward(&edit);
      break;
    case CommandDeleteWordBackward:
      EditDeleteWordBackward(&edit);
      break;
    case CommandUndo:
      Report(EditUndo(&edit), "nothing to undo");
      break;
    case CommandRedo:
      Report(EditRedo(&edit), "nothing to redo");
      break;
    case CommandCopy:
      Copy(false);
      break;
    case CommandCut:
      Copy(true);
      break;
    case CommandPaste:
      Paste();
      break;
    case CommandMoveLinesUp:
      EditMoveLines(&edit, true);
      break;
    case CommandMoveLinesDown:
      EditMoveLines(&edit, false);
      break;
    case CommandDuplicateLine:
      EditDuplicateLines(&edit);
      break;
    case CommandDeleteLine:
      EditDeleteLines(&edit);
      break;
    case CommandToggleComment:
      ToggleComment();
      break;
    case CommandSortLines:
      AskSortOrder();
      break;
    case CommandJoinLines:
      Report(EditJoinLines(&edit), "no line below to join");
      break;
    case CommandTrimLines:
      TrimWhitespace(false);
      break;
    case CommandTrimWhitespace:
      TrimWhitespace(true);
      break;
    case CommandUpperCase:
      ChangeCase(true);
      break;
    case CommandLowerCase:
      ChangeCase(false);
      break;
    case CommandAlignTable:
      AskTableWidth();
      break;
    case CommandAlignCursors:
      AlignCursors();
      break;
    default:
      return false;
  }

  return true;
}

static void
ExpandSelection(void)
{
  Document *document = Current();
  if (MoveExpandSelection(document->view, document->highlight)) return;

  bool syntax = document->highlight && !HighlightDisabled(document->highlight);
  StrBufAppendString(&message, syntax ? "no larger syntax node" : "no brackets around");
}

// shows or hides the change panel or the diagnostics panel, which take the same place, so
// showing one hides the other.
static void
TogglePanel(bool changes)
{
  i32 width = windowSize.width;
  bool other = changes ? IssuesPanelOpen() : ReviewPanelOpen();

  if (other && changes) IssuesTogglePanel(width);
  else if (other) ReviewTogglePanel(width);

  if (changes) ReviewTogglePanel(width);
  else IssuesTogglePanel(width);
}

static bool
NavigationCommand(enum Command command)
{
  switch (command) {
    case CommandExpandSelection:
      ExpandSelection();
      break;
    case CommandShrinkSelection:
      Report(MoveShrinkSelection(CurrentView()), "no expanded selection to shrink");
      break;
    case CommandSelectionTopUp:
      Report(MoveSelectionEdge(CurrentView(), MoveEdgeTop, true), "no line above");
      break;
    case CommandSelectionBottomDown:
      Report(MoveSelectionEdge(CurrentView(), MoveEdgeBottom, false), "no line below");
      break;
    case CommandSelectionTopDown:
      Report(MoveSelectionEdge(CurrentView(), MoveEdgeTop, false), "no line to drop");
      break;
    case CommandSelectionBottomUp:
      Report(MoveSelectionEdge(CurrentView(), MoveEdgeBottom, true), "no line to drop");
      break;
    case CommandJumpBack:
      JumpBack();
      break;
    case CommandJumpForward:
      JumpForward();
      break;
    case CommandFileSymbols:
      SymbolsPickFile();
      break;
    case CommandNextSymbol:
      SymbolsStep(false);
      break;
    case CommandPreviousSymbol:
      SymbolsStep(true);
      break;
    case CommandGoToDefinition:
      LookupDefinition();
      break;
    case CommandShowSignature:
      LookupSignature(windowSize.width);
      break;
    case CommandFindReferences:
      LookupReferences();
      break;
    case CommandOtherFile:
      LookupOtherFile();
      break;
    case CommandRename:
      RefactorRename();
      break;
    case CommandFix:
      RefactorFix();
      break;
    case CommandFormat:
      RefactorFormat(expandTab);
      break;
    case CommandProjectSymbols:
      SymbolsPickProject();
      break;
    case CommandNextChange:
      ReviewStep(false);
      break;
    case CommandPreviousChange:
      ReviewStep(true);
      break;
    case CommandChangePanel:
      TogglePanel(true);
      break;
    case CommandRevertChange:
      ReviewRevert();
      break;
    case CommandReviewMultifile:
      MultifileReview();
      break;
    case CommandPickChanged:
      ReviewPickChanged();
      break;
    case CommandNextDiagnostic:
      IssuesStep(false);
      break;
    case CommandPreviousDiagnostic:
      IssuesStep(true);
      break;
    case CommandDiagnosticPanel:
      TogglePanel(false);
      break;
    case CommandPickDiagnostic:
      IssuesPick();
      break;
    default:
      return false;
  }

  return true;
}

static void
RunCommand(const Binding *binding);

static void
HelpPicked([[maybe_unused]] void *context, bool picked, u64 id)
{
  if (picked) RunCommand(&keymapBindings[id]);
}

static i32
CompareHelp(const void *a, const void *b)
{
  const Binding *first = &keymapBindings[*(const u32 *)a];
  const Binding *second = &keymapBindings[*(const u32 *)b];
  return strcmp(first->help, second->help);
}

// lists every binding by what it does, and runs the one picked.
static void
ShowHelp(void)
{
  u32 *order = MemoryAllocate(keymapBindingCount * sizeof(u32));
  u64 keyWidth = 0;
  StrBuf line = {};

  for (u32 index = 0; index < keymapBindingCount; index++) {
    order[index] = index;
    StrBufClear(&line);
    KeyName(keymapBindings[index].key, &line);
    if (line.length > keyWidth) keyWidth = line.length;
  }

  qsort(order, keymapBindingCount, sizeof(u32), CompareHelp);
  PickerBegin();

  for (u32 index = 0; index < keymapBindingCount; index++) {
    const Binding *binding = &keymapBindings[order[index]];
    StrBufClear(&line);
    KeyName(binding->key, &line);
    while (line.length < keyWidth + 2) StrBufAppendByte(&line, ' ');
    StrBufAppendString(&line, binding->help);
    PickerAdd(line.data, line.length, nullptr, order[index]);
  }

  StrBufFree(&line);
  free(order);
  PickerOpen("shortcuts", "shortcut: ", HelpPicked, nullptr);
}

static bool
BufferCommand(enum Command command)
{
  switch (command) {
    case CommandNewBuffer:
      SessionNew();
      break;
    case CommandOpen:
      SessionOpen();
      break;
    case CommandLoadNewFiles:
      SessionLoadNew();
      break;
    case CommandNextBuffer:
      SessionCycle(1);
      break;
    case CommandPreviousBuffer:
      SessionCycle(-1);
      break;
    case CommandLastBuffer:
      SessionLastUsed();
      break;
    case CommandPickBuffer:
      SessionPick(false);
      break;
    case CommandPickUnsaved:
      SessionPick(true);
      break;
    case CommandCloseBuffer:
      SessionClose();
      break;
    case CommandCloseFolder:
      SessionCloseFolder();
      break;
    case CommandFileMenu:
      SaveFileMenu(&workspace, &message);
      break;
    case CommandSave:
      SaveCurrent(&workspace, &message, nullptr);
      break;
    case CommandSaveAll:
      SaveAll(&workspace, &message, nullptr);
      break;
    case CommandQuit:
      SessionQuit(Quit);
      break;
    case CommandSplitSame:
      SessionSplit(false, windowSize.width);
      break;
    case CommandSplitNew:
      SessionSplit(true, windowSize.width);
      break;
    case CommandFocusOtherView:
      SessionFocusOther();
      break;
    case CommandSwapViews:
      SessionSwapViews();
      break;
    case CommandCloseView:
      SessionCloseView();
      break;
    default:
      return false;
  }

  return true;
}

static bool
SearchCommand(enum Command command)
{
  switch (command) {
    case CommandFind:
      SearchFind();
      break;
    case CommandFindNext:
      SearchStep(false);
      break;
    case CommandFindPrevious:
      SearchStep(true);
      break;
    case CommandJumpWord:
      SearchWord();
      break;
    case CommandReplace:
      SearchReplace();
      break;
    case CommandSpellCheck:
      SearchSpell();
      break;
    case CommandSpellWord:
      SearchSpellWord();
      break;
    case CommandProjectSearch:
      SearchProject(ProjectAsk);
      break;
    case CommandProjectResults:
      ProjectShow();
      break;
    default:
      return false;
  }

  return true;
}

static void
RunCommand(const Binding *binding)
{
  enum Command command = binding->command;
  View *view = CurrentView();

  // only consecutive backspaces or deletes join one undo step.
  bool deleting = command == CommandDeleteBackward || command == CommandDeleteForward;
  if (!deleting) UndoBreak(&Current()->undo);

  if (MoveCommand(command) || EditCommand(command) || BufferCommand(command)) return;
  if (SearchCommand(command) || NavigationCommand(command)) return;

  switch (command) {
    case CommandScrollUp:
      Scroll(-1);
      break;
    case CommandScrollDown:
      Scroll(1);
      break;
    case CommandSelectAll:
      MoveSelectAll(view);
      break;
    case CommandSelectBrackets:
      Report(MoveSelectBrackets(view), "no brackets around");
      break;
    case CommandJumpBracket:
      Report(MoveToMatchingBracket(view), "no matching bracket");
      break;
    case CommandCancel:
      Cancel();
      break;
    case CommandAddCursorWord:
      MultiCursorAddNext(view, SearchMarks(Current()), &message);
      break;
    case CommandSkipCursorWord:
      MultiCursorSkip(view, SearchMarks(Current()), &message);
      break;
    case CommandAddCursorAll:
      MultiCursorAddAll(view, SearchMarks(Current()), &message);
      break;
    case CommandAddCursorAbove:
      Report(MultiCursorAddRow(view, true), "no line above");
      break;
    case CommandAddCursorBelow:
      Report(MultiCursorAddRow(view, false), "no line below");
      break;
    case CommandReload:
      AskToReload();
      break;
    case CommandToggleLineEndings:
      ToggleLineEndings();
      break;
    case CommandCopyPath:
      CopyPath();
      break;
    case CommandOpenTerminal:
      OpenTerminal();
      break;
    case CommandStatistics:
      ShowStatistics();
      break;
    case CommandGoToLine:
      PromptAsk("go to line: ", "", GoToAnswered, nullptr);
      break;
    case CommandToggleExpandTab:
      Toggle(&expandTab, "expand tab");
      break;
    case CommandToggleSoftWrap:
      Toggle(&displayOptions.softWrap, "soft wrap");
      break;
    case CommandToggleLineNumbers:
      Toggle(&displayOptions.lineNumbers, "line numbers");
      break;
    case CommandToggleWhitespace:
      Toggle(&displayOptions.showWhitespace, "whitespace");
      break;
    case CommandToggleRuler:
      Toggle(&displayOptions.ruler, "ruler");
      break;
    case CommandToggleServers:
      ToggleServers();
      break;
    case CommandSetRulerColumn:
      AskRulerColumn();
      break;
    case CommandHelp:
      ShowHelp();
      break;
    case CommandLearn:
      LearnStart();
      break;

    // the rest were run above.
    default:
      break;
  }
}

static void
HandleKey(Key key)
{
  // a running scan (project search, indexing symbols) only listens for escape, and keeps
  // its progress showing.
  if (ScanBusy()) {
    ScanKey(key);
    return;
  }

  StrBufClear(&message);

  // learning mode answers with keys, so none of them runs a command.
  if (LearnActive()) {
    LearnKey(key);
    return;
  }

  // a picker reads its filter through a prompt, so it gets the keys first, all but soft
  // wrap, which its preview follows.
  if (PickerActive()) {
    const Binding *binding = KeymapFind(key);
    if (binding && binding->command == CommandToggleSoftWrap) RunCommand(binding);
    else PickerKey(key);
    return;
  }

  // so do the find and replace prompts.
  if (SearchActive()) {
    SearchKey(key);
    return;
  }

  if (PromptActive()) {
    PromptKey(key);
    return;
  }

  if (KeyIsText(key)) {
    char text[4];
    EditContext edit = Editing();
    EditInsert(&edit, text, Utf8Encode(key.code, text));
    return;
  }

  const Binding *binding = KeymapFind(key);
  if (binding) RunCommand(binding);
}

static void
HandlePaste(const char *text, u64 length)
{
  if (ScanBusy() || LearnActive()) return;

  if (PromptActive()) {
    PromptPaste(text, length);
    return;
  }

  // a terminal paste is one undo step and is never indented.
  StrBufClear(&message);
  UndoBreak(&Current()->undo);
  EditContext edit = Editing();
  EditPaste(&edit, text, length, false);
}

// the view the last frame drew at a screen cell, and its document, or nullptr when it drew
// none there (or the split it was part of has closed since).
static View *
ViewAt(i32 row, i32 column, Document **document)
{
  u32 pane = RenderPaneAt(row, column);
  if (pane == renderNoPane) return nullptr;

  if (pane == drawnFocus) {
    *document = Current();
    return CurrentView();
  }

  if (!workspace.split) return nullptr;

  *document = workspace.entries[workspace.other].document;
  return WorkspaceOtherView(&workspace);
}

static void
HandleMouse(const MouseEvent *mouse)
{
  i32 row = mouse->row;
  i32 column = mouse->column;
  bool press = mouse->button == MouseButtonLeft && mouse->action == MouseActionPress;
  bool wheel = mouse->button == MouseButtonWheelUp || mouse->button == MouseButtonWheelDown;
  i64 rows = mouse->button == MouseButtonWheelUp ? -wheelRows : wheelRows;

  // a scan, learning mode and the prompts take the keys and leave the mouse alone, but a
  // picker takes clicks and the wheel on its list.
  if (ScanBusy() || LearnActive()) return;
  if (press) StrBufClear(&message);

  if (PickerActive()) {
    if (press) PickerClick(row, column);
    else if (wheel) PickerWheel(row, column, rows);
    return;
  }

  if (SearchActive() || PromptActive()) return;

  if (mouse->action == MouseActionRelease) {
    MouseRelease();
    return;
  }

  // a drag stays in the view it started in, which the press gave focus, wherever the
  // pointer goes; past an edge of the view it reaches the text beyond, scrolling to it.
  if (mouse->action == MouseActionDrag) {
    if (mouse->button == MouseButtonLeft) MouseDrag(CurrentView(), row, column);
    return;
  }

  // a press on the status bar of the other side of a split moves the focus there.
  u32 status = RenderStatusAt(row, column);

  if (status != renderNoPane) {
    if (press && status != drawnFocus && workspace.split) SessionFocusOther();
    return;
  }

  Document *document;
  View *view = ViewAt(row, column, &document);
  if (!view) return;

  // the wheel scrolls the view under the pointer without giving it focus.
  // cursors carried along, like cursors put somewhere else, end a run of typing.
  if (wheel) {
    UndoBreak(&document->undo);
    MoveScroll(view, rows);
    return;
  }

  // alt adds a cursor, or removes one, and ctrl+alt selects columns.
  if (!press) return;

  enum MouseMode mode = MouseModePlace;
  if (mouse->modifiers == KeyModifierAlt) mode = MouseModeAdd;
  else if (mouse->modifiers == (KeyModifierAlt | KeyModifierControl)) mode = MouseModeColumn;
  else if (mouse->modifiers) return;

  // a press on the other side of a split moves the focus there first, as ctrl+j does.
  UndoBreak(&document->undo);
  if (view != CurrentView()) SessionFocusOther();
  MousePress(view, row, column, mode);
}

static void
HandleEvent(const InputEvent *event)
{
  if (event->type != InputEventMouse) MouseInterrupt();

  if (event->type == InputEventKey) HandleKey(event->key);
  else if (event->type == InputEventPaste) HandlePaste(event->text, event->length);
  else if (event->type == InputEventMouse) HandleMouse(&event->mouse);
}

static void
Resize(void)
{
  windowSize = TermGetSize();
  ScreenResize(windowSize.width, windowSize.height);
}

// waits for input, or while a scan runs only checks for it.
static void
WaitForInput(void)
{
  bool pending = InputPending();
  i32 timeout = pending ? escapeTimeout : -1;
  if (ScanBusy() && !pending) timeout = 0;

  // a request running out of time, or the cursor resting long enough for the diagnostics
  // panel to ask what is under it, wakes the loop too.
  i32 waits[] = {LspTimeout(), IssuesTimeout()};

  for (u32 index = 0; index < 2; index++) {
    if (waits[index] >= 0 && (timeout < 0 || waits[index] < timeout)) timeout = waits[index];
  }

  // language servers wake the loop too, and LspPump handles what they sent.
  struct pollfd servers[lspMaxPollFds];
  u32 serverCount = LspPollFds(servers);

  switch (TermWait(timeout, servers, serverCount)) {
    case TermWaitInput:
      TermReadInput();
      break;
    case TermWaitResize:
      Resize();
      break;
    case TermWaitTimeout:
      if (pending) InputExpire();
      break;
    case TermWaitOther:
      break;
  }
}

// what one side of the window shows.
typedef struct Pane {
  StrBuf name;
  StrBuf type;
  StrBuf changes;
  StrBuf diagnostics;
  StatusInfo status;
  MatchSet marks;
} Pane;

static void
DescribePane(Pane *pane, u64 index, View *view, FramePane *framePane)
{
  const Document *document = workspace.entries[index].document;
  WorkspaceName(&workspace, index, &pane->name);

  // the language, noting when the buffer is too large or slow to colour.
  if (document->lang) StrBufAppendString(&pane->type, document->lang->name);
  if (HighlightDisabled(document->highlight)) StrBufAppendString(&pane->type, " (uncoloured)");
  LspDescribe(document->lsp, &pane->type);

  // the lines changed since the last commit, when there are any.
  ChangeCounts counts = ChangesCount(document->changes);
  bool changed = counts.added || counts.changed || counts.removed;
  if (changed) {
    StrBuf *text = &pane->changes;
    StrBufAppendFormat(text, "+%lu ~%lu -%lu", counts.added, counts.changed, counts.removed);
  }

  // the errors and warnings the language server reports, when there are any.
  const Diagnostics *diagnostics = LspDiagnostics(document->lsp);
  DiagnosticCounts problems = diagnostics ? DiagnosticsCount(diagnostics)
                                          : (DiagnosticCounts){};
  if (problems.errors) StrBufAppendFormat(&pane->diagnostics, "E%u", problems.errors);
  if (problems.errors && problems.warnings) StrBufAppendByte(&pane->diagnostics, ' ');
  if (problems.warnings) StrBufAppendFormat(&pane->diagnostics, "W%u", problems.warnings);

  pane->status = (StatusInfo){
    .name = &pane->name,
    .modified = DocumentModified(document),
    .readOnly = document->file.readOnly,
    .fileType = pane->type.data,
    .lineEnding = FileLineEndingName(document->file.lineEnding),
    .changes = changed ? pane->changes.data : nullptr,
    .diagnostics = pane->diagnostics.length ? pane->diagnostics.data : nullptr,
    .openCount = workspace.openCount,
    .loadedCount = workspace.count
  };

  pane->marks = SearchMarks(document);

  *framePane = (FramePane){
    .view = view,
    .highlight = document->highlight,
    .changes = document->changes,
    .diagnostics = diagnostics,
    .marks = &pane->marks,
    .status = &pane->status
  };
}

static void
DrawPanel([[maybe_unused]] void *context, i32 column, i32 width, i32 height)
{
  if (ReviewPanelOpen()) ReviewDrawPanel(column, width, height);
  else IssuesDrawPanel(column, width, height);
}

static void
Draw(void)
{
  Frame frame = {
    .width = windowSize.width,
    .height = windowSize.height,
    .paneCount = 1,
    .message = &message
  };

  Pane panes[2] = {};
  DescribePane(&panes[0], workspace.current, CurrentView(), &frame.panes[0]);

  if (workspace.split) {
    DescribePane(&panes[1], workspace.other, WorkspaceOtherView(&workspace), &frame.panes[1]);
    frame.paneCount = 2;

    // panes are given left to right.
    if (workspace.slot != workspace.left) {
      FramePane focused = frame.panes[0];
      frame.panes[0] = frame.panes[1];
      frame.panes[1] = focused;
      frame.focused = 1;
    }
  }

  if (ReviewPanelOpen() || IssuesPanelOpen()) frame.panel = (FramePanel){DrawPanel, nullptr};
  RenderFrame(&frame);
  drawnFocus = frame.focused;

  for (u32 index = 0; index < 2; index++) {
    StrBufFree(&panes[index].name);
    StrBufFree(&panes[index].type);
    StrBufFree(&panes[index].changes);
    StrBufFree(&panes[index].diagnostics);
  }

  // a picker covers the view and its status bar.
  i32 width = windowSize.width;
  i32 height = windowSize.height;
  bool roomy = width >= minWindowWidth && height >= minWindowHeight;
  if (roomy && PickerActive()) PickerDraw(width, height);
  if (roomy && PromptActive()) PromptDraw(height - 1, width);
  ScreenFlush();
}

i32
AppRun(i32 argc, char **argv)
{
  // files are loaded before the terminal changes, so argument errors leave it untouched.
  LangLoadAll(&message);
  LspInit(&message);
  WorkspaceInit(&workspace, &displayOptions);
  SearchInit(&workspace, &message);
  ScanInit(&workspace, &message);
  PreviewInit(&workspace);
  ProjectInit(&workspace, &message);
  JumpInit(&workspace, &message);
  SymbolsInit(&workspace, &message);
  ReviewInit(&workspace, &message);
  IssuesInit(&workspace, &message);
  LookupInit(&workspace, &message);
  PlacesInit(&workspace, &message);
  RefactorInit(&workspace, &message);
  MultifileInit(&workspace, &message);
  LearnInit(&message);
  SessionStart(&workspace, &message, argv + 1, (u64)argc - 1);

  TermStart();
  Resize();

  while (running) {
    // handle everything already buffered, then draw once.
    InputEvent event;
    while (running && InputNext(&event)) HandleEvent(&event);

    // a scan runs a slice at a time, so its progress shows and escape stops it.
    if (running && ScanBusy()) ScanContinue();

    // sends the edits just made, and takes in what the servers said, before drawing.
    if (running) IssuesTick();
    if (running) LspPump();
    if (running) Draw();
    if (running) WaitForInput();
  }

  // the servers are given time to exit once the terminal is back to normal.
  TermStop();
  LspStopAll();
  return EXIT_SUCCESS;
}
