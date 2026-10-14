#include "keycheck.h"

#include "fatal.h"
#include "keymap.h"
#include "screen.h"
#include "settings.h"
#include "strbuf.h"
#include "utf8.h"

static constexpr u32 logCapacity = 128;
static constexpr u64 maxRawShown = 48;
static constexpr u64 maxTextLength = 200;

static const Style titleStyle = {colorDefault, colorDefault, StyleAttributeReverse};
static const Style labelStyle = {245, colorDefault, 0};
static const Style unseenStyle = {215, colorDefault, 0};
static const Style unboundStyle = {204, colorDefault, 0};

typedef struct LogLine {
  StrBuf text;
  bool unbound;
} LogLine;

// most recent entries are at the end, the oldest is dropped when full.
static LogLine logLines[logCapacity];
static u32 logCount;

static bool seen[128];
static u32 resizeCount;
static StrBuf typed;

static LogLine *
AddLogLine(void)
{
  if (logCount == logCapacity) {
    LogLine oldest = logLines[0];
    for (u32 index = 1; index < logCapacity; index++) logLines[index - 1] = logLines[index];
    logLines[logCapacity - 1] = oldest;
    logCount--;
  }

  LogLine *line = &logLines[logCount++];
  StrBufClear(&line->text);
  line->unbound = false;
  return line;
}

static void
AppendRaw(StrBuf *line, const char *raw, u64 length)
{
  u64 shown = length < maxRawShown ? length : maxRawShown;

  for (u64 index = 0; index < shown; index++) {
    u8 byte = (u8)raw[index];
    if (byte == 0x1B) StrBufAppendString(line, "\\e");
    else if (byte < 0x20 || byte == 0x7F) StrBufAppendFormat(line, "^%c", byte ^ 0x40);
    else StrBufAppendByte(line, (char)byte);
  }

  if (shown < length) StrBufAppendString(line, "...");
}

static void
AppendPadded(StrBuf *line, u64 start, u64 width)
{
  while (line->length < start + width) StrBufAppendByte(line, ' ');
  StrBufAppendByte(line, ' ');
}

static void
EditTyped(Key key)
{
  if (KeyIsText(key) && typed.length < maxTextLength) {
    char encoded[4];
    StrBufAppend(&typed, encoded, Utf8Encode(key.code, encoded));
    return;
  }

  if (key.code == KeyBackspace && key.modifiers == 0 && typed.length) {
    // step back over continuation bytes to the start of the last code point.
    u64 end = typed.length - 1;
    while (end > 0 && ((u8)typed.data[end] & 0xC0) == 0x80) end--;
    StrBufRemove(&typed, end, typed.length - end);
  }

  if (key.code == KeyEnter && key.modifiers == 0) StrBufClear(&typed);
}

static bool
HandleKey(const InputEvent *event)
{
  Key key = event->key;

  if (KeyEqual(key, (Key){'q', KeyModifierControl | KeyModifierAlt})) {
    Fatal("forced fatal error from the key check screen");
  }

  LogLine *line = AddLogLine();
  StrBuf *text = &line->text;
  KeyName(key, text);
  AppendPadded(text, 0, 22);

  const Binding *binding = KeymapFind(key);
  u64 start = text->length;
  line->unbound = !binding && !KeyIsText(key);

  if (binding) {
    u64 index = (u64)(binding - keymapBindings);
    if (index < sizeof seen) seen[index] = true;
    StrBufAppendString(text, binding->help);
  } else {
    StrBufAppendString(text, line->unbound ? "(unbound)" : "(text)");
  }

  AppendPadded(text, start, 56);
  AppendRaw(text, event->raw, event->rawLength);
  EditTyped(key);
  return !(binding && binding->command == CommandQuit);
}

bool
KeyCheckHandle(const InputEvent *event)
{
  if (event->type == InputEventKey) return HandleKey(event);
  if (event->type != InputEventPaste) return true;

  u64 lines = 1;
  for (u64 index = 0; index < event->length; index++) lines += event->text[index] == '\n';

  StrBuf *text = &AddLogLine()->text;
  StrBufAppendFormat(text, "paste: %lu bytes, %lu lines: ", event->length, lines);
  AppendRaw(text, event->text, event->length);
  return true;
}

void
KeyCheckResized(void)
{
  resizeCount++;
}

static i32
DrawUnseen(i32 width, i32 lastRow)
{
  // lists the bindings not yet pressed, wrapped across rows.
  u32 unseen = 0;
  for (u32 index = 0; index < keymapBindingCount; index++) unseen += !seen[index];

  StrBuf label = {};
  StrBufAppendFormat(&label, "unseen bindings: %u of %u", unseen, keymapBindingCount);
  ScreenPutString(1, 0, label.data, labelStyle);
  StrBufFree(&label);

  i32 row = 2;
  i32 column = 0;
  StrBuf name = {};

  for (u32 index = 0; index < keymapBindingCount && row <= lastRow; index++) {
    if (seen[index]) continue;

    StrBufClear(&name);
    KeyName(keymapBindings[index].key, &name);
    i32 length = (i32)name.length;

    if (column > 0 && column + length > width) {
      row++;
      column = 0;
    }

    if (row <= lastRow) column = ScreenPutString(row, column, name.data, unseenStyle) + 2;
  }

  StrBufFree(&name);
  return column > 0 ? row + 1 : row;
}

void
KeyCheckDraw(i32 width, i32 height)
{
  ScreenBegin();

  if (width < minWindowWidth || height < minWindowHeight) {
    ScreenPutString(0, 0, "terminal too small", styleDefault);
    return;
  }

  StrBuf title = {};
  StrBufAppendFormat(
    &title,
    " key check   %dx%d   resizes %u   ctrl+q quits   ctrl+alt+q forces a fatal error",
    width,
    height,
    resizeCount
  );
  ScreenFill(0, 0, width, titleStyle);
  ScreenPutString(0, 0, title.data, titleStyle);
  StrBufFree(&title);

  // the unseen list takes at most a third of the screen, the log gets the rest.
  i32 logTop = DrawUnseen(width, height / 3) + 1;
  i32 logBottom = height - 3;
  i32 row = logBottom;

  for (u32 index = logCount; index > 0 && row >= logTop; index--, row--) {
    const LogLine *line = &logLines[index - 1];
    Style style = line->unbound ? unboundStyle : styleDefault;
    ScreenPutText(row, 0, line->text.data, line->text.length, style);
  }

  i32 column = ScreenPutString(height - 1, 0, "type here: ", labelStyle);
  column = ScreenPutText(height - 1, column, typed.data, typed.length, styleDefault);
  ScreenSetCursor(height - 1, column);
}
