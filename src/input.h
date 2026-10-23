#pragma once

// decodes the byte stream from foot into input events.
// it understands the kitty keyboard protocol with the disambiguate flag as foot sends it
// (with the cursor keys, home, end, page up and down, insert and delete in their csi
// forms), bracketed paste, sgr mouse reports, focus reports and the replies to the startup
// queries.
// the decoder only parses bytes; term.c reads them and feeds them in.
#include "key.h"
#include "types.h"

enum InputEventType {
  InputEventKey,
  InputEventPaste,
  InputEventCursorReport,
  InputEventKeyboardReport,
  InputEventMouse,

  // the window has gained focus.
  InputEventFocus
};

enum MouseButton {
  MouseButtonLeft,
  MouseButtonMiddle,
  MouseButtonRight,
  MouseButtonWheelUp,
  MouseButtonWheelDown,

  // a sideways wheel, an extra button, or motion with no button held.
  MouseButtonOther
};

enum MouseAction {
  MouseActionPress,

  // motion while the button is held.
  MouseActionDrag,
  MouseActionRelease
};

// one mouse report: the button, the KeyModifier bits held, and the screen cell (from 0).
typedef struct MouseEvent {
  enum MouseButton button;
  enum MouseAction action;
  u8 modifiers;
  i32 row;
  i32 column;
} MouseEvent;

typedef struct InputEvent {
  enum InputEventType type;

  // InputEventKey.
  Key key;

  // InputEventPaste: the pasted bytes, exactly as sent.
  const char *text;
  u64 length;

  // InputEventCursorReport: 1-based position.
  i32 row;
  i32 column;

  // InputEventMouse.
  MouseEvent mouse;

  // the bytes the event was decoded from.
  const char *raw;
  u64 rawLength;
} InputEvent;

// adds bytes read from the terminal.
// pointers held by earlier events are invalid after this call.
void
InputFeed(const char *data, u64 length);

// decodes the next complete event.
// returns false when no complete event is buffered.
bool
InputNext(InputEvent *event);

// true when the buffer ends in an incomplete escape sequence that may never be finished.
// an unfinished paste does not count, because the rest of it is still on its way.
bool
InputPending(void);

// drops an incomplete sequence after a timeout.
void
InputExpire(void);
