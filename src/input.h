#pragma once

// decodes the byte stream from the terminal into input events.
// it understands the kitty keyboard protocol, the legacy sequences terminals still send
// for some keys under it, bracketed paste, sgr mouse reports and the replies to terminal
// queries.
// the decoder only parses bytes; term.c reads them and feeds them in.
#include "key.h"
#include "types.h"

enum InputEventType {
  InputEventKey,
  InputEventPaste,
  InputEventCursorReport,
  InputEventKeyboardReport,
  InputEventMouse
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

  // InputEventKeyboardReport: the active kitty keyboard protocol flags.
  u32 flags;

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

// resolves an incomplete sequence after a timeout.
// a lone escape byte becomes the escape key, anything else is dropped.
void
InputExpire(void);
