#include "app.h"

#include <stdlib.h>

#include "buffer.h"
#include "fatal.h"
#include "file.h"
#include "input.h"
#include "keycheck.h"
#include "keymap.h"
#include "screen.h"
#include "term.h"

// how long an incomplete escape sequence may wait for the rest of its bytes.
static constexpr i32 escapeTimeout = 50;

static TermSize windowSize;

// until buffer lists arrive in M5 the editor holds a single buffer.
static Buffer buffer;
static File file;

// a save refused because the file changed on disk is forced by saving again.
static bool overwritePending;

static void
ShowMessage(const StrBuf *message)
{
  KeyCheckSetMessage(message->data, message->length);
}

static void
DescribeBuffer(StrBuf *message)
{
  const char *name = file.path.length ? file.path.data : "[unnamed]";

  StrBufAppendFormat(
    message,
    "%s%s: %lu lines, %s",
    name,
    file.readOnly ? " [RO]" : "",
    BufferLineCount(&buffer),
    FileLineEndingName(file.lineEnding)
  );
}

static void
LoadArguments(i32 argumentCount, char **arguments, StrBuf *message)
{
  BufferInit(&buffer);
  if (argumentCount < 2) return;

  // several files and folders arrive with buffer lists in M5.
  if (argumentCount > 2) Fatal("only one file can be opened for now");

  StrBufAppendString(&file.path, arguments[1]);
  enum FileStatus status = FileLoad(&file, &buffer, message);

  // a file that cannot be opened leaves an unnamed buffer, with the reason shown.
  if (status != FileStatusOk && status != FileStatusNew) {
    FileFree(&file);
    return;
  }

  if (message->length) StrBufAppendString(message, "; ");
  DescribeBuffer(message);
  if (status == FileStatusNew) StrBufAppendString(message, " (new file)");
}

static void
Save(void)
{
  StrBuf message = {};
  enum FileStatus status = FileSave(&file, &buffer, overwritePending, &message);
  overwritePending = status == FileStatusChanged;

  if (status == FileStatusOk) {
    StrBufAppendString(&message, "saved ");
    DescribeBuffer(&message);
  }

  if (overwritePending) StrBufAppendString(&message, "; ctrl+s again to overwrite");
  ShowMessage(&message);
  StrBufFree(&message);
}

static bool
HandleEvent(const InputEvent *event)
{
  const Binding *binding = event->type == InputEventKey ? KeymapFind(event->key) : nullptr;

  if (binding && binding->command == CommandSave) Save();
  else if (event->type == InputEventKey) overwritePending = false;

  return KeyCheckHandle(event);
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
    KeyCheckResized();
    break;
  case TermWaitTimeout:
    InputExpire();
    break;
  }
}

i32
AppRun(i32 argumentCount, char **arguments)
{
  // files are read before the terminal changes, so argument errors leave it untouched.
  StrBuf message = {};
  LoadArguments(argumentCount, arguments, &message);

  TermStart();
  Resize();
  ShowMessage(&message);
  StrBufFree(&message);

  bool running = true;

  while (running) {
    // handle everything already buffered, then draw once.
    InputEvent event;
    while (running && InputNext(&event)) running = HandleEvent(&event);

    KeyCheckDraw(windowSize.width, windowSize.height);
    ScreenFlush();
    if (running) WaitForInput();
  }

  TermStop();
  return EXIT_SUCCESS;
}
