#include "app.h"

#include <stdlib.h>
#include <sys/stat.h>

#include "clipboard.h"
#include "document.h"
#include "edit.h"
#include "fatal.h"
#include "input.h"
#include "keymap.h"
#include "move.h"
#include "prompt.h"
#include "render.h"
#include "screen.h"
#include "settings.h"
#include "term.h"
#include "utf8.h"
#include "view.h"

// how long an incomplete escape sequence may wait for the rest of its bytes.
static constexpr i32 escapeTimeout = 50;

static TermSize windowSize;

// until buffer lists arrive in M5 the editor holds a single document.
static Document document;
static View view;

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

// runs once a save started by SaveDocument succeeds.
typedef void (*SaveFinished)(void);
static SaveFinished afterSave;

// the document had no name before the save as prompt gave it one.
static bool namedForSave;

static EditContext
Editing(void)
{
  return (EditContext){.view = &view, .undo = &document.undo, .expandTab = expandTab};
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
  if (!ViewScroll(&view, count)) StrBufAppendString(&message, "cannot scroll further");
}

// a save that fails after the save as prompt leaves the document unnamed again.
static void
ForgetNewName(void)
{
  if (namedForSave) StrBufClear(&document.file.path);
  namedForSave = false;
}

static void WriteDocument(bool overwriteChanged);

static void
OverwriteAnswered(
  [[maybe_unused]] void *context,
  const char *answer,
  [[maybe_unused]] u64 length
) {
  if (answer && *answer == 'y') {
    WriteDocument(true);
    return;
  }

  ForgetNewName();
  StrBufAppendString(&message, "not saved");
}

static void
WriteDocument(bool overwriteChanged)
{
  enum FileStatus status = DocumentSave(&document, overwriteChanged, &message);

  if (status == FileStatusChanged) {
    StrBufClear(&message);
    const char *question = "the file changed on disk; overwrite it? (y/n)";
    PromptChoose(question, "yn", OverwriteAnswered, nullptr);
    return;
  }

  if (status != FileStatusOk) {
    ForgetNewName();
    return;
  }

  namedForSave = false;
  StrBufAppendFormat(&message, "saved %lu lines", BufferLineCount(&document.buffer));
  if (afterSave) afterSave();
}

static void
SaveAsAnswered([[maybe_unused]] void *context, const char *answer, u64 length)
{
  if (!answer || !length) {
    StrBufAppendString(&message, "not saved");
    return;
  }

  StrBufAppend(&document.file.path, answer, length);
  namedForSave = true;

  // an existing file is only replaced once confirmed.
  struct stat status;

  if (lstat(document.file.path.data, &status) == 0) {
    StrBufClear(&message);
    PromptChoose("the file exists; overwrite it? (y/n)", "yn", OverwriteAnswered, nullptr);
    return;
  }

  WriteDocument(false);
}

// saves the document, asking for a name first when it has none, then runs finished.
static void
SaveDocument(SaveFinished finished)
{
  afterSave = finished;

  if (trimTrailingWhitespace) {
    EditContext edit = Editing();
    EditTrimTrailingWhitespace(&edit);
  }

  if (document.file.path.length) WriteDocument(false);
  else PromptAsk("save as: ", "", SaveAsAnswered, nullptr);
}

static void
Quit(void)
{
  running = false;
}

static void
QuitAnswered(
  [[maybe_unused]] void *context,
  const char *answer,
  [[maybe_unused]] u64 length
) {
  if (answer && *answer == 's') SaveDocument(Quit);
  else if (answer && *answer == 'd') Quit();
}

static void
AskToQuit(void)
{
  if (!DocumentModified(&document)) {
    Quit();
    return;
  }

  PromptChoose("unsaved changes: (s)ave, (d)iscard, (c)ancel", "sdc", QuitAnswered, nullptr);
}

static void
Reload(void)
{
  if (!DocumentReload(&document, &message)) return;

  ViewClamp(&view);
  StrBufAppendFormat(&message, "reloaded %lu lines", BufferLineCount(&document.buffer));
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
  if (!document.file.path.length) {
    StrBufAppendString(&message, "the buffer has no file to reload");
    return;
  }

  if (!DocumentModified(&document)) {
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

  MoveGoTo(&view, line - 1, column - 1);
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
  File *file = &document.file;
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
  bool lineWise = cut ? EditCut(&edit, &text) : EditCopy(&view, &text);
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

// escape clears a selection, or collapses several cursors to one.
static void
Cancel(void)
{
  CursorSet *set = &view.cursors;

  if (set->count > 1) {
    CursorSetCollapse(set);
    return;
  }

  Cursor *cursor = CursorSetPrimary(set);
  cursor->anchor = cursor->head;
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
    MoveCursors(&view, moves[command - CommandMoveUp], false);
    return true;
  }

  if (command >= CommandSelectUp && command <= CommandSelectFileEnd) {
    MoveCursors(&view, moves[command - CommandSelectUp], true);
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

static void
RunCommand(const Binding *binding)
{
  enum Command command = binding->command;

  // only consecutive backspaces or deletes join one undo step.
  bool deleting = command == CommandDeleteBackward || command == CommandDeleteForward;
  if (!deleting) UndoBreak(&document.undo);

  if (MoveCommand(command) || EditCommand(command)) return;

  switch (command) {
  case CommandScrollUp: Scroll(-1); break;
  case CommandScrollDown: Scroll(1); break;
  case CommandSelectAll: MoveSelectAll(&view); break;
  case CommandSelectBrackets: Report(MoveSelectBrackets(&view), "no brackets around"); break;
  case CommandJumpBracket: Report(MoveToMatchingBracket(&view), "no matching bracket"); break;
  case CommandCancel: Cancel(); break;
  case CommandReload: AskToReload(); break;
  case CommandToggleLineEndings: ToggleLineEndings(); break;
  case CommandGoToLine: PromptAsk("go to line: ", "", GoToAnswered, nullptr); break;
  case CommandToggleExpandTab: Toggle(&expandTab, "expand tab"); break;
  case CommandToggleSoftWrap: Toggle(&displayOptions.softWrap, "soft wrap"); break;
  case CommandToggleLineNumbers: Toggle(&displayOptions.lineNumbers, "line numbers"); break;
  case CommandToggleWhitespace: Toggle(&displayOptions.showWhitespace, "whitespace"); break;
  case CommandToggleRuler: Toggle(&displayOptions.ruler, "ruler"); break;
  case CommandSetRulerColumn: AskRulerColumn(); break;
  case CommandSave: SaveDocument(nullptr); break;
  case CommandQuit: AskToQuit(); break;
  default: StrBufAppendFormat(&message, "not available yet: %s", binding->help); break;
  }
}

static void
HandleKey(Key key)
{
  StrBufClear(&message);

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
  UndoBreak(&document.undo);
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
  StatusInfo status = {
    .name = &document.file.path,
    .modified = DocumentModified(&document),
    .readOnly = document.file.readOnly,
    .lineEnding = FileLineEndingName(document.file.lineEnding),
    .openCount = 1,
    .loadedCount = 1
  };

  Frame frame = {
    .width = windowSize.width,
    .height = windowSize.height,
    .view = &view,
    .status = &status,
    .message = &message
  };

  RenderFrame(&frame);
  bool roomy = windowSize.width >= minWindowWidth && windowSize.height >= minWindowHeight;
  if (roomy && PromptActive()) PromptDraw(windowSize.height - 1, windowSize.width);
  ScreenFlush();
}

i32
AppRun(i32 argumentCount, char **arguments)
{
  // several files and folders arrive with buffer lists in M5.
  if (argumentCount > 2) Fatal("only one file can be opened for now");

  // files are read before the terminal changes, so argument errors leave it untouched.
  DocumentOpen(&document, argumentCount > 1 ? arguments[1] : nullptr, &message);
  ViewInit(&view, &document.buffer, &displayOptions);

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
