#include "key.h"

#include "utf8.h"

typedef struct KeyLabel {
  u32 code;
  const char *name;
} KeyLabel;

static const KeyLabel keyLabels[] = {
  {KeyTab, "tab"},
  {KeyEnter, "enter"},
  {KeyEscape, "esc"},
  {KeySpace, "space"},
  {KeyBackspace, "backspace"},
  {KeyUp, "up"},
  {KeyDown, "down"},
  {KeyLeft, "left"},
  {KeyRight, "right"},
  {KeyHome, "home"},
  {KeyEnd, "end"},
  {KeyPageUp, "pgup"},
  {KeyPageDown, "pgdn"},
  {KeyInsert, "insert"},
  {KeyDelete, "delete"}
};

bool
KeyEqual(Key a, Key b)
{
  return a.code == b.code && a.modifiers == b.modifiers;
}

bool
KeyIsText(Key key)
{
  constexpr u8 commandModifiers = KeyModifierControl | KeyModifierAlt | KeyModifierSuper;
  if (key.modifiers & commandModifiers) return false;
  return key.code >= KeySpace && key.code < 0x110000 && key.code != KeyBackspace;
}

void
KeyName(Key key, StrBuf *name)
{
  // modifiers are named in the order used by the help list.
  if (key.modifiers & KeyModifierControl) StrBufAppendString(name, "ctrl+");
  if (key.modifiers & KeyModifierAlt) StrBufAppendString(name, "alt+");
  if (key.modifiers & KeyModifierShift) StrBufAppendString(name, "shift+");
  if (key.modifiers & KeyModifierSuper) StrBufAppendString(name, "super+");

  for (u32 index = 0; index < sizeof(keyLabels) / sizeof(keyLabels[0]); index++) {
    if (keyLabels[index].code != key.code) continue;

    StrBufAppendString(name, keyLabels[index].name);
    return;
  }

  if (key.code < KeySpace || key.code > 0x10FFFF) {
    StrBufAppendFormat(name, "key%u", key.code);
    return;
  }

  char encoded[4];
  StrBufAppend(name, encoded, Utf8Encode(key.code, encoded));
}
