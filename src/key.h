#pragma once

// key presses as decoded from the terminal.
// a key is a code plus a set of modifiers.
// text keys use their unicode code point (always the unshifted form when a modifier other
// than shift is held), and keys without a code point use the KeyCode values below.
#include "strbuf.h"
#include "types.h"

enum KeyCode : u32 {
  // these match the code points the kitty keyboard protocol uses for them.
  KeyTab = 9,
  KeyEnter = 13,
  KeyEscape = 27,
  KeySpace = 32,
  KeyBackspace = 127,

  // the rest sit above the unicode range so they never collide with text.
  KeyUp = 0x110000,
  KeyDown,
  KeyLeft,
  KeyRight,
  KeyHome,
  KeyEnd,
  KeyPageUp,
  KeyPageDown,
  KeyInsert,
  KeyDelete
};

enum KeyModifier : u8 {
  KeyModifierShift = 1,
  KeyModifierAlt = 2,
  KeyModifierControl = 4,
  KeyModifierSuper = 8
};

typedef struct Key {
  u32 code;
  u8 modifiers;
} Key;

bool
KeyEqual(Key a, Key b);

// true for a key that inserts its code point as text (no control, alt or super).
bool
KeyIsText(Key key);

// appends the display name of a key, such as "ctrl+shift+up" or "alt+.".
void
KeyName(Key key, StrBuf *name);
