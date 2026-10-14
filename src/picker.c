#include "picker.h"

#include <string.h>

#include "layout.h"
#include "memory.h"
#include "prompt.h"
#include "screen.h"
#include "strbuf.h"
#include "theme.h"
#include "utf8.h"

static constexpr u64 noNote = UINT64_MAX;

// an item's strings live in the shared buffers below, by offset.
typedef struct Item {
  u64 text;
  u64 length;

  // the text case folded, for filtering.
  u64 folded;
  u64 foldedLength;

  u64 note;
  u64 id;
} Item;

static StrBuf texts;
static StrBuf foldedTexts;
static Item *items;
static u64 itemCount;
static u64 itemCapacity;

// the items that pass the filter, as indexes into items, in list order.
static u64 *matches;
static u64 matchCount;

static bool active;
static StrBuf title;
static PickerDone onDone;
static void *doneContext;

// the selected match, the first match shown, and how many rows the list had last drawn.
static u64 selected;
static u64 top;
static u64 pageRows = 1;

// appends text with every code point case folded.
static void
Fold(const char *text, u64 length, StrBuf *output)
{
  u64 at = 0;

  while (at < length) {
    u8 byte = (u8)text[at];

    if (byte < 0x80) {
      StrBufAppendByte(output, (char)(byte >= 'A' && byte <= 'Z' ? byte | 0x20 : byte));
      at++;
      continue;
    }

    u32 codepoint;
    at += Utf8Decode(text + at, length - at, &codepoint);
    char encoded[4];
    StrBufAppend(output, encoded, Utf8Encode(Utf8Fold(codepoint), encoded));
  }
}

void
PickerBegin(void)
{
  // reserved so an item's text always points into memory, even when every text is empty.
  StrBufReserve(&texts, 64);
  StrBufReserve(&foldedTexts, 64);
  StrBufClear(&texts);
  StrBufClear(&foldedTexts);
  itemCount = 0;
}

void
PickerAdd(const char *text, u64 length, const char *note, u64 id)
{
  if (itemCount == itemCapacity) {
    itemCapacity = itemCapacity ? itemCapacity * 2 : 256;
    items = MemoryResize(items, itemCapacity, sizeof(Item));
    matches = MemoryResize(matches, itemCapacity, sizeof(u64));
  }

  Item *item = &items[itemCount++];
  *item = (Item){.text = texts.length, .length = length, .note = noNote, .id = id};
  StrBufAppend(&texts, text, length);

  item->folded = foldedTexts.length;
  Fold(text, length, &foldedTexts);
  item->foldedLength = foldedTexts.length - item->folded;

  if (note) {
    item->note = texts.length;
    StrBufAppend(&texts, note, strlen(note) + 1);
  }
}

static bool
Contains(const char *text, u64 length, const char *part, u64 partLength)
{
  if (partLength > length) return false;
  if (!partLength) return true;

  for (u64 at = 0; at + partLength <= length; at++) {
    const char *first = memchr(text + at, part[0], length - partLength + 1 - at);
    if (!first) return false;

    at = (u64)(first - text);
    if (memcmp(first, part, partLength) == 0) return true;
  }

  return false;
}

static bool
Passes(const Item *item, const char *filter, u64 length, bool start, bool end)
{
  const char *text = foldedTexts.data + item->folded;
  u64 textLength = item->foldedLength;
  if (!start && !end) return Contains(text, textLength, filter, length);
  if (length > textLength) return false;
  if (start && end) return length == textLength && memcmp(text, filter, length) == 0;
  if (start) return memcmp(text, filter, length) == 0;
  return memcmp(text + textLength - length, filter, length) == 0;
}

static void
Filter([[maybe_unused]] void *context, const char *filter, u64 length)
{
  StrBuf folded = {};
  Fold(filter, length, &folded);

  // the anchors are the only special characters, and are not matched themselves.
  const char *part = folded.data ? folded.data : "";
  u64 partLength = folded.length;
  bool start = partLength && part[0] == '^';
  bool end = partLength > (u64)start && part[partLength - 1] == '$';
  part += start;
  partLength -= (u64)start + (u64)end;

  matchCount = 0;

  for (u64 index = 0; index < itemCount; index++) {
    if (Passes(&items[index], part, partLength, start, end)) matches[matchCount++] = index;
  }

  selected = 0;
  top = 0;
  StrBufFree(&folded);
}

static void
Answered([[maybe_unused]] void *context, const char *answer, [[maybe_unused]] u64 length)
{
  bool picked = answer && matchCount;
  u64 id = picked ? items[matches[selected]].id : 0;
  active = false;
  onDone(doneContext, picked, id);
}

void
PickerOpen(const char *newTitle, const char *label, PickerDone done, void *context)
{
  StrBufClear(&title);
  StrBufAppendString(&title, newTitle);
  onDone = done;
  doneContext = context;
  active = true;
  Filter(nullptr, "", 0);

  PromptAsk(label, "", Answered, nullptr);
  PromptHandle(Filter, nullptr);
}

bool
PickerActive(void)
{
  return active;
}

static void
Select(i64 step)
{
  if (!matchCount) return;

  i64 target = (i64)selected + step;
  if (target < 0) target = 0;
  if (target >= (i64)matchCount) target = (i64)matchCount - 1;
  selected = (u64)target;
}

void
PickerKey(Key key)
{
  i64 page = (i64)pageRows;
  i64 all = (i64)matchCount;

  if (key.modifiers) {
    PromptKey(key);
    return;
  }

  switch (key.code) {
  case KeyUp: Select(-1); break;
  case KeyDown: Select(1); break;
  case KeyPageUp: Select(-page); break;
  case KeyPageDown: Select(page); break;
  case KeyHome: Select(-all); break;
  case KeyEnd: Select(all); break;

  // enter with nothing listed has nothing to pick, so the picker stays open.
  case KeyEnter: if (matchCount) PromptKey(key); break;
  default: PromptKey(key); break;
  }
}

static void
DrawItem(const Item *item, i32 row, i32 width, bool isSelected)
{
  Style style = isSelected ? ThemeStyle(ThemeSelection) : styleDefault;
  Style noteStyle = ThemeStyle(ThemeGutter);
  noteStyle.background = style.background;
  ScreenFill(row, 0, width, style);

  const char *note = item->note == noNote ? nullptr : texts.data + item->note;
  i32 noteWidth = note ? (i32)LayoutColumn(note, strlen(note), strlen(note)) + 1 : 0;

  // a space either side, and the note after the text.
  StrBuf shown = {};
  LayoutShorten(texts.data + item->text, item->length, width - 2 - noteWidth, &shown);
  i32 column = ScreenPutText(row, 1, shown.data, shown.length, style);
  if (note) ScreenPutString(row, column + 1, note, noteStyle);
  StrBufFree(&shown);
}

void
PickerDraw(i32 width, i32 height)
{
  // the bar above the message bar names the list and counts the matches.
  i32 listRows = height - 2;
  pageRows = listRows > 1 ? (u64)listRows : 1;
  if (selected < top) top = selected;
  if (selected >= top + pageRows) top = selected - pageRows + 1;

  for (i32 row = 0; row < listRows; row++) {
    u64 index = top + (u64)row;
    ScreenFill(row, 0, width, styleDefault);
    if (index < matchCount) DrawItem(&items[matches[index]], row, width, index == selected);
  }

  Style bar = ThemeStyle(ThemeStatus);
  StrBuf count = {};
  StrBufAppendFormat(&count, "%lu/%lu ", matchCount, itemCount);
  ScreenFill(listRows, 0, width, bar);
  ScreenPutText(listRows, 1, title.data, title.length, bar);
  ScreenPutText(listRows, width - (i32)count.length, count.data, count.length, bar);
  StrBufFree(&count);
}
