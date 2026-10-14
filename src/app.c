#include "app.h"

#include <stdlib.h>
#include <string.h>

#include "clipboard.h"
#include "document.h"
#include "edit.h"
#include "input.h"
#include "keymap.h"
#include "memory.h"
#include "move.h"
#include "multicursor.h"
#include "picker.h"
#include "prompt.h"
#include "render.h"
#include "save.h"
#include "screen.h"
#include "search.h"
#include "session.h"
#include "settings.h"
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

// the message bar text, cleared by the next key.
static StrBuf message;

static bool running = true;

// the current document and its view.
// fetched afresh by every command, since opening a folder frees every document.
static Document *
Current(void)
{
  return WorkspaceDocument(&workspace);
}

static View *
CurrentView(void)
{
  return &Current()->view;
}

static EditContext
Editing(void)
{
  Document *document = Current();
  View *view = &document->view;
  return (EditContext){.view = view, .undo = &document->undo, .expandTab = expandTab};
}

static void
Toggle(bool *setting, const char *name)
{
  *setting = !*setting;
  StrBufAppendFormat(&message, "%s %s", name, *setting ? "on" : "off");
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
ReloadAnswered(
  [[maybe_unused]] void *context,
  const char *answer,
  [[maybe_unused]] u64 length
) {
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
  case CommandNewline: EditNewline(&edit); break;
  case CommandIndent: EditIndent(&edit); break;
  case CommandDedent: EditDedent(&edit); break;
  case CommandDeleteBackward: EditDeleteBackward(&edit); break;
  case CommandDeleteForward: EditDeleteForward(&edit); break;
  case CommandDeleteWordBackward: EditDeleteWordBackward(&edit); break;
  case CommandUndo: Report(EditUndo(&edit), "nothing to undo"); break;
  case CommandRedo: Report(EditRedo(&edit), "nothing to redo"); break;
  case CommandCopy: Copy(false); break;
  case CommandCut: Copy(true); break;
  case CommandPaste: Paste(); break;
  case CommandMoveLinesUp: EditMoveLines(&edit, true); break;
  case CommandMoveLinesDown: EditMoveLines(&edit, false); break;
  case CommandDuplicateLine: EditDuplicateLines(&edit); break;
  case CommandDeleteLine: EditDeleteLines(&edit); break;
  default: return false;
  }

  return true;
}

static void RunCommand(const Binding *binding);

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
  case CommandNewBuffer: SessionNew(); break;
  case CommandOpen: SessionOpen(); break;
  case CommandNextBuffer: SessionCycle(1); break;
  case CommandPreviousBuffer: SessionCycle(-1); break;
  case CommandLastBuffer: SessionLastUsed(); break;
  case CommandPickBuffer: SessionPick(false); break;
  case CommandPickUnsaved: SessionPick(true); break;
  case CommandSave: SaveCurrent(&workspace, &message, nullptr); break;
  case CommandSaveAll: SaveAll(&workspace, &message, nullptr); break;
  case CommandQuit: SessionQuit(Quit); break;
  default: return false;
  }

  return true;
}

static bool
SearchCommand(enum Command command)
{
  switch (command) {
  case CommandFind: SearchFind(); break;
  case CommandFindNext: SearchStep(false); break;
  case CommandFindPrevious: SearchStep(true); break;
  case CommandJumpWord: SearchWord(); break;
  case CommandReplace: SearchReplace(); break;
  case CommandSpellCheck: SearchSpell(); break;
  default: return false;
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
  if (SearchCommand(command)) return;

  switch (command) {
  case CommandScrollUp: Scroll(-1); break;
  case CommandScrollDown: Scroll(1); break;
  case CommandSelectAll: MoveSelectAll(view); break;
  case CommandSelectBrackets: Report(MoveSelectBrackets(view), "no brackets around"); break;
  case CommandJumpBracket: Report(MoveToMatchingBracket(view), "no matching bracket"); break;
  case CommandCancel: Cancel(); break;
  case CommandAddCursorWord: MultiCursorAddNext(view, &message); break;
  case CommandAddCursorAbove: Report(MultiCursorAddRow(view, true), "no line above"); break;
  case CommandAddCursorBelow: Report(MultiCursorAddRow(view, false), "no line below"); break;
  case CommandReload: AskToReload(); break;
  case CommandToggleLineEndings: ToggleLineEndings(); break;
  case CommandGoToLine: PromptAsk("go to line: ", "", GoToAnswered, nullptr); break;
  case CommandToggleExpandTab: Toggle(&expandTab, "expand tab"); break;
  case CommandToggleSoftWrap: Toggle(&displayOptions.softWrap, "soft wrap"); break;
  case CommandToggleLineNumbers: Toggle(&displayOptions.lineNumbers, "line numbers"); break;
  case CommandToggleWhitespace: Toggle(&displayOptions.showWhitespace, "whitespace"); break;
  case CommandToggleRuler: Toggle(&displayOptions.ruler, "ruler"); break;
  case CommandSetRulerColumn: AskRulerColumn(); break;
  case CommandHelp: ShowHelp(); break;
  default: StrBufAppendFormat(&message, "not available yet: %s", binding->help); break;
  }
}

static void
HandleKey(Key key)
{
  StrBufClear(&message);

  // a picker reads its filter through a prompt, so it gets the keys first.
  if (PickerActive()) {
    PickerKey(key);
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

static void
HandleEvent(const InputEvent *event)
{
  if (event->type == InputEventKey) HandleKey(event->key);
  else if (event->type == InputEventPaste) HandlePaste(event->text, event->length);
}

static void
Resize(void)
{
  windowSize = TermGetSize();
  ScreenResize(windowSize.width, windowSize.height);
}

static void
WaitForInput(void)
{
  switch (TermWait(InputPending() ? escapeTimeout : -1)) {
  case TermWaitInput:
    TermReadInput();
    break;
  case TermWaitResize:
    Resize();
    break;
  case TermWaitTimeout:
    InputExpire();
    break;
  }
}

static void
Draw(void)
{
  Document *document = Current();
  StrBuf name = {};
  WorkspaceName(&workspace, workspace.current, &name);

  StatusInfo status = {
    .name = &name,
    .modified = DocumentModified(document),
    .readOnly = document->file.readOnly,
    .lineEnding = FileLineEndingName(document->file.lineEnding),
    .openCount = workspace.openCount,
    .loadedCount = workspace.count
  };

  MatchSet marks = SearchMarks(document);

  Frame frame = {
    .width = windowSize.width,
    .height = windowSize.height,
    .view = &document->view,
    .marks = &marks,
    .status = &status,
    .message = &message
  };

  RenderFrame(&frame);
  StrBufFree(&name);

  // a picker covers the view and its status bar.
  i32 width = windowSize.width;
  i32 height = windowSize.height;
  bool roomy = width >= minWindowWidth && height >= minWindowHeight;
  if (roomy && PickerActive()) PickerDraw(width, height);
  if (roomy && PromptActive()) PromptDraw(height - 1, width);
  ScreenFlush();
}

i32
AppRun(i32 argumentCount, char **arguments)
{
  // files are loaded before the terminal changes, so argument errors leave it untouched.
  WorkspaceInit(&workspace, &displayOptions);
  SearchInit(&workspace, &message);
  SessionStart(&workspace, &message, arguments + 1, (u64)argumentCount - 1);

  TermStart();
  Resize();

  while (running) {
    // handle everything already buffered, then draw once.
    InputEvent event;
    while (running && InputNext(&event)) HandleEvent(&event);

    if (running) Draw();
    if (running) WaitForInput();
  }

  TermStop();
  return EXIT_SUCCESS;
}
