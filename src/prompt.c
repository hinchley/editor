#include "prompt.h"

#include <string.h>

#include "layout.h"
#include "screen.h"
#include "strbuf.h"
#include "theme.h"
#include "utf8.h"

enum PromptKind {
  PromptKindClosed,
  PromptKindText,
  PromptKindChoice
};

static enum PromptKind kind;
static StrBuf label;
static StrBuf text;
static StrBuf choices;
static PromptDone onDone;
static PromptChanged onChange;
static PromptComplete onComplete;
static void *doneContext;

// the byte offset of the cursor in text, and the first column shown when text is too
// wide for the row.
static u64 cursor;
static u64 scroll;

// the last key or paste changed the text.
static bool edited;

static void
Open(enum PromptKind newKind, const char *newLabel, PromptDone done, void *context)
{
  kind = newKind;
  StrBufClear(&label);
  StrBufAppendString(&label, newLabel);
  StrBufClear(&text);
  cursor = 0;
  scroll = 0;
  onDone = done;
  onChange = nullptr;
  onComplete = nullptr;
  doneContext = context;
}

void
PromptAsk(const char *newLabel, const char *initial, PromptDone done, void *context)
{
  Open(PromptKindText, newLabel, done, context);
  StrBufAppendString(&text, initial);
  cursor = text.length;
}

void
PromptHandle(PromptChanged changed, PromptComplete complete)
{
  onChange = changed;
  onComplete = complete;
}

void
PromptSetLabel(const char *newLabel)
{
  StrBufClear(&label);
  StrBufAppendString(&label, newLabel);
}

void
PromptSetText(const char *newText, u64 length)
{
  StrBufClear(&text);
  StrBufAppend(&text, newText, length);
  cursor = text.length;
  if (onChange) onChange(doneContext, text.data ? text.data : "", text.length);
}

const StrBuf *
PromptText(void)
{
  return &text;
}

void
PromptChoose(const char *question, const char *newChoices, PromptDone done, void *context)
{
  Open(PromptKindChoice, question, done, context);
  StrBufClear(&choices);
  StrBufAppendString(&choices, newChoices);
}

bool
PromptActive(void)
{
  return kind != PromptKindClosed;
}

// closes the prompt, then hands the answer over.
static void
Finish(const char *answer, u64 length)
{
  // the answer is copied, since the callback may open a new prompt over the old text.
  StrBuf copy = {};
  if (answer) StrBufAppend(&copy, answer, length);

  const char *copied = copy.data ? copy.data : "";
  kind = PromptKindClosed;
  onDone(doneContext, answer ? copied : nullptr, copy.length);
  StrBufFree(&copy);
}

// inserts text at the cursor, leaving out control characters, which cannot be shown.
static void
Insert(const char *data, u64 length)
{
  for (u64 index = 0; index < length; index++) {
    u8 byte = (u8)data[index];
    if (byte < 0x20 || byte == 0x7F) continue;

    StrBufInsert(&text, cursor, &data[index], 1);
    cursor++;
    edited = true;
  }
}

static u64
WordStart(u64 at)
{
  while (at > 0 && text.data[at - 1] == ' ') at--;
  while (at > 0 && text.data[at - 1] != ' ') at--;
  return at;
}

static void
DeleteTo(u64 at)
{
  StrBufRemove(&text, at, cursor - at);
  edited |= at != cursor;
  cursor = at;
}

static void
DeleteNext(u64 next)
{
  StrBufRemove(&text, cursor, next - cursor);
  edited |= next != cursor;
}

static void
Complete(void)
{
  if (!onComplete) return;

  onComplete(doneContext, &text);
  cursor = text.length;
  edited = true;
}

static void
Edit(Key key)
{
  u64 previous = cursor ? Utf8PreviousBoundary(text.data, text.length, cursor) : 0;
  u64 next = cursor < text.length ? Utf8NextBoundary(text.data, text.length, cursor) : cursor;
  bool control = key.modifiers & KeyModifierControl;

  if (KeyIsText(key)) {
    char encoded[4];
    Insert(encoded, Utf8Encode(key.code, encoded));
    return;
  }

  if (key.modifiers & ~(KeyModifierControl | KeyModifierShift)) return;

  switch (key.code) {
  case KeyEnter: Finish(text.data ? text.data : "", text.length); break;
  case KeyEscape: Finish(nullptr, 0); break;
  case KeyLeft: cursor = previous; break;
  case KeyRight: cursor = next; break;
  case KeyHome: cursor = 0; break;
  case KeyEnd: cursor = text.length; break;
  case KeyBackspace: DeleteTo(control ? WordStart(cursor) : previous); break;
  case KeyDelete: DeleteNext(next); break;
  case KeyTab: Complete(); break;
  default: break;
  }
}

// edits the text and reports a change.
// a key that closes the prompt changes nothing, so a prompt opened by its callback is
// never told about a change it did not see.
static void
TextKey(Key key)
{
  edited = false;
  Edit(key);
  if (edited && onChange) onChange(doneContext, text.data ? text.data : "", text.length);
}

static void
ChoiceKey(Key key)
{
  if (key.code == KeyEscape && !key.modifiers) {
    Finish(nullptr, 0);
    return;
  }

  if (!KeyIsText(key) || key.code >= 0x80) return;

  // a capital letter that is a choice of its own is kept, otherwise it means the
  // lowercase one.
  char typed = (char)key.code;
  char lower = (char)(key.code | 0x20);
  char chosen = strchr(choices.data, typed) ? typed : lower;
  if (!strchr(choices.data, chosen)) return;

  Finish(&chosen, 1);
}

void
PromptKey(Key key)
{
  if (kind == PromptKindText) TextKey(key);
  else if (kind == PromptKindChoice) ChoiceKey(key);
}

void
PromptPaste(const char *data, u64 length)
{
  if (kind != PromptKindText) return;

  const char *newline = memchr(data, '\n', length);
  if (newline) length = (u64)(newline - data);
  edited = false;
  Insert(data, length);
  if (edited && onChange) onChange(doneContext, text.data, text.length);
}

void
PromptDraw(i32 row, i32 width)
{
  Style style = ThemeStyle(ThemeMessage);
  ScreenFill(row, 0, width, style);
  i32 column = ScreenPutText(row, 0, label.data, label.length, style);

  if (kind == PromptKindChoice) {
    ScreenSetCursor(row, column);
    return;
  }

  // scroll sideways to keep the cursor inside the room left after the label.
  i64 room = width - column - 1;
  u64 available = room > 1 ? (u64)room : 1;
  const char *data = text.data ? text.data : "";
  u64 cursorColumn = LayoutColumn(data, text.length, cursor);
  if (cursorColumn < scroll) scroll = cursorColumn;
  if (cursorColumn >= scroll + available) scroll = cursorColumn - available + 1;

  LayoutRow whole = {0, text.length, true};
  u64 start = LayoutByteAt(data, text.length, whole, scroll);
  u64 startColumn = LayoutColumn(data, text.length, start);
  ScreenPutText(row, column, data + start, text.length - start, style);
  ScreenSetCursor(row, column + (i32)(cursorColumn - startColumn));
}
