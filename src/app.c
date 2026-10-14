#include "app.h"

#include <stdlib.h>

#include "buffer.h"
#include "fatal.h"
#include "file.h"
#include "input.h"
#include "keymap.h"
#include "move.h"
#include "render.h"
#include "screen.h"
#include "settings.h"
#include "term.h"
#include "view.h"

// how long an incomplete escape sequence may wait for the rest of its bytes.
static constexpr i32 escapeTimeout = 50;

static TermSize windowSize;

// until buffer lists arrive in M5 the editor holds a single buffer.
static Buffer buffer;
static File file;
static View view;

static DisplayOptions displayOptions = {
  .softWrap = softWrapDefault,
  .lineNumbers = lineNumbersDefault,
  .showWhitespace = showWhitespaceDefault,
  .ruler = rulerDefault,
  .rulerColumn = rulerColumnDefault
};

// the message bar text, cleared by the next key.
static StrBuf message;

// a save refused because the file changed on disk is forced by saving again.
static bool overwritePending;

static void
LoadArguments(i32 argumentCount, char **arguments)
{
  BufferInit(&buffer);
  if (argumentCount < 2) return;

  // several files and folders arrive with buffer lists in M5.
  if (argumentCount > 2) Fatal("only one file can be opened for now");

  StrBufAppendString(&file.path, arguments[1]);
  enum FileStatus status = FileLoad(&file, &buffer, &message);

  // a file that cannot be opened leaves an unnamed buffer, with the reason shown.
  if (status != FileStatusOk && status != FileStatusNew) FileFree(&file);
  if (status == FileStatusNew) StrBufAppendString(&message, "new file");
}

static void
Save(void)
{
  enum FileStatus status = FileSave(&file, &buffer, overwritePending, &message);
  overwritePending = status == FileStatusChanged;

  if (status == FileStatusOk) {
    StrBufAppendFormat(&message, "saved %lu lines", BufferLineCount(&buffer));
  }

  if (overwritePending) StrBufAppendString(&message, "; ctrl+s again to overwrite");
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

static bool
MoveCommand(enum Command command)
{
  static const enum Move moves[] = {
    [CommandMoveUp] = MoveUp,
    [CommandMoveDown] = MoveDown,
    [CommandMoveLeft] = MoveLeft,
    [CommandMoveRight] = MoveRight,
    [CommandMoveLineStart] = MoveLineStart,
    [CommandMoveLineEnd] = MoveLineEnd,
    [CommandMovePageUp] = MovePageUp,
    [CommandMovePageDown] = MovePageDown,
    [CommandMoveWordLeft] = MoveWordLeft,
    [CommandMoveWordRight] = MoveWordRight,
    [CommandMoveFileStart] = MoveFileStart,
    [CommandMoveFileEnd] = MoveFileEnd
  };

  if (command > CommandMoveFileEnd) return false;
  MoveCursors(&view, moves[command], false);
  return true;
}

// runs a command and returns false when the editor should quit.
static bool
RunCommand(const Binding *binding)
{
  enum Command command = binding->command;
  if (command != CommandSave) overwritePending = false;
  if (MoveCommand(command)) return true;

  switch (command) {
  case CommandScrollUp: Scroll(-1); break;
  case CommandScrollDown: Scroll(1); break;
  case CommandToggleSoftWrap: Toggle(&displayOptions.softWrap, "soft wrap"); break;
  case CommandToggleLineNumbers: Toggle(&displayOptions.lineNumbers, "line numbers"); break;
  case CommandToggleWhitespace: Toggle(&displayOptions.showWhitespace, "whitespace"); break;
  case CommandToggleRuler: Toggle(&displayOptions.ruler, "ruler"); break;
  case CommandSave: Save(); break;
  case CommandQuit: return false;
  default: StrBufAppendFormat(&message, "not available yet: %s", binding->help); break;
  }

  return true;
}

static bool
HandleEvent(const InputEvent *event)
{
  if (event->type != InputEventKey) return true;

  StrBufClear(&message);
  const Binding *binding = KeymapFind(event->key);
  if (binding) return RunCommand(binding);

  overwritePending = false;
  return true;
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
    .name = &file.path,
    .readOnly = file.readOnly,
    .lineEnding = FileLineEndingName(file.lineEnding),
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
  ScreenFlush();
}

i32
AppRun(i32 argumentCount, char **arguments)
{
  // files are read before the terminal changes, so argument errors leave it untouched.
  LoadArguments(argumentCount, arguments);
  ViewInit(&view, &buffer, &displayOptions);

  TermStart();
  Resize();
  bool running = true;

  while (running) {
    // handle everything already buffered, then draw once.
    InputEvent event;
    while (running && InputNext(&event)) running = HandleEvent(&event);

    if (running) Draw();
    if (running) WaitForInput();
  }

  TermStop();
  return EXIT_SUCCESS;
}
