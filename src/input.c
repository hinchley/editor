#include "input.h"

#include <string.h>

#include "strbuf.h"
#include "utf8.h"

enum ParseResult {
  ParseEvent,
  ParseSkip,
  ParseIncomplete
};

// the bytes being decoded and what came of them.
typedef struct Parse {
  const char *text;
  u64 length;
  u64 size;
  InputEvent *event;
} Parse;

// a csi sequence split into its parts.
// parameters are separated by ';' and sub parameters by ':'.
// a missing parameter reads as 0.
typedef struct Csi {
  u8 marker;
  u8 final;
  u32 fields[4][3];
} Csi;

// a keypad key foot reports with a private use code point.
typedef struct KeypadKey {
  u32 code;
  u32 key;
} KeypadKey;

static const KeypadKey keypadKeys[] = {
  {57399, '0'},
  {57400, '1'},
  {57401, '2'},
  {57402, '3'},
  {57403, '4'},
  {57404, '5'},
  {57405, '6'},
  {57406, '7'},
  {57407, '8'},
  {57408, '9'},
  {57409, '.'},
  {57410, '/'},
  {57411, '*'},
  {57412, '-'},
  {57413, '+'},
  {57414, KeyEnter},
  {57415, '='},
  {57416, ','},
  {57417, KeyLeft},
  {57418, KeyRight},
  {57419, KeyUp},
  {57420, KeyDown},
  {57421, KeyPageUp},
  {57422, KeyPageDown},
  {57423, KeyHome},
  {57424, KeyEnd},
  {57425, KeyInsert},
  {57426, KeyDelete}
};

static constexpr char pasteEnd[] = "\x1b[201~";
static constexpr u64 pasteEndLength = sizeof(pasteEnd) - 1;

// a sequence longer than this without a final byte is garbage.
static constexpr u64 maxSequenceLength = 256;

// bytes not yet decoded start at inputStart.
static StrBuf input;
static u64 inputStart;

// an unfinished paste and how far it has been searched for its end marker.
static bool pasteOpen;
static u64 pasteScanned;

// set by InputExpire so the next InputNext resolves an incomplete sequence.
static bool inputExpired;

static u8
DecodeModifiers(u32 value)
{
  // the protocol sends one plus a bit set, and 0 when the parameter is absent.
  // lock keys and the rarely used hyper and meta bits are dropped.
  if (value <= 1) return 0;
  return (u8)((value - 1) &
    (KeyModifierShift | KeyModifierAlt | KeyModifierControl | KeyModifierSuper));
}

static enum ParseResult
KeyEvent(Parse *parse, u32 code, u8 modifiers)
{
  if (code == 0) return ParseSkip;

  parse->event->type = InputEventKey;
  parse->event->key = (Key){code, modifiers};
  return ParseEvent;
}

// a key reported as CSI code u.
// foot always reports the unshifted key, and sends a shifted letter as plain text.
static enum ParseResult
KittyKey(Parse *parse, u32 code, u8 modifiers)
{
  for (u32 index = 0; index < sizeof(keypadKeys) / sizeof(keypadKeys[0]); index++) {
    const KeypadKey *keypad = &keypadKeys[index];
    if (keypad->code == code) return KeyEvent(parse, keypad->key, modifiers);
  }

  // other private use codes are functional keys the editor has no use for.
  if (code >= 57344 && code <= 63743) return ParseSkip;
  return KeyEvent(parse, code, modifiers);
}

// a key reported as CSI number ~.
// F3 and above are reported this way too, and are skipped.
static u32
TildeKey(u32 number)
{
  switch (number) {
    case 2:
      return KeyInsert;
    case 3:
      return KeyDelete;
    case 5:
      return KeyPageUp;
    case 6:
      return KeyPageDown;
    default:
      return 0;
  }
}

// a key reported as CSI 1 ; modifiers letter, or CSI letter without modifiers.
// F1, F2 and F4 are reported this way too, and are skipped.
static u32
LetterKey(u8 final)
{
  switch (final) {
    case 'A':
      return KeyUp;
    case 'B':
      return KeyDown;
    case 'C':
      return KeyRight;
    case 'D':
      return KeyLeft;
    case 'H':
      return KeyHome;
    case 'F':
      return KeyEnd;
    default:
      return 0;
  }
}

static enum ParseResult
CursorReport(Parse *parse, const Csi *csi)
{
  parse->event->type = InputEventCursorReport;
  parse->event->row = (i32)csi->fields[0][0];
  parse->event->column = (i32)csi->fields[1][0];
  return ParseEvent;
}

static enum ParseResult
Mouse(Parse *parse, const Csi *csi, bool released)
{
  // the first parameter packs the button into its low two bits, with a bit each for
  // shift, alt, ctrl, motion and the wheel above them.
  // the cell is 1-based.
  u32 code = csi->fields[0][0];
  u32 column = csi->fields[1][0];
  u32 row = csi->fields[2][0];
  if (!column || !row) return ParseSkip;

  static constexpr enum MouseButton buttons[] =
    {MouseButtonLeft, MouseButtonMiddle, MouseButtonRight, MouseButtonOther};
  static constexpr enum MouseButton wheels[] =
    {MouseButtonWheelUp, MouseButtonWheelDown, MouseButtonOther, MouseButtonOther};

  enum MouseButton button = buttons[code & 3];
  if (code & 64) button = wheels[code & 3];
  if (code & 128) button = MouseButtonOther;

  u8 modifiers = 0;
  if (code & 4) modifiers |= KeyModifierShift;
  if (code & 8) modifiers |= KeyModifierAlt;
  if (code & 16) modifiers |= KeyModifierControl;

  enum MouseAction action = MouseActionPress;
  if (code & 32) action = MouseActionDrag;
  if (released) action = MouseActionRelease;

  parse->event->type = InputEventMouse;
  parse->event->mouse = (MouseEvent){button, action, modifiers, (i32)row - 1, (i32)column - 1};
  return ParseEvent;
}

static enum ParseResult
Paste(Parse *parse)
{
  // the text runs from the end of the start marker to the end marker.
  u64 start = parse->size;
  u64 from = pasteScanned > start ? pasteScanned : start;

  for (u64 index = from; index + pasteEndLength <= parse->length; index++) {
    if (parse->text[index] != '\x1b') continue;
    if (memcmp(parse->text + index, pasteEnd, pasteEndLength) != 0) continue;

    pasteOpen = false;
    pasteScanned = 0;
    parse->size = index + pasteEndLength;
    parse->event->type = InputEventPaste;
    parse->event->text = parse->text + start;
    parse->event->length = index - start;
    return ParseEvent;
  }

  // keep the tail in case it holds the start of the end marker.
  pasteOpen = true;
  pasteScanned = parse->length >= pasteEndLength ? parse->length - pasteEndLength + 1 : 0;
  return ParseIncomplete;
}

static enum ParseResult
InterpretCsi(Parse *parse, const Csi *csi)
{
  u32 first = csi->fields[0][0];
  u8 modifiers = DecodeModifiers(csi->fields[1][0]);

  if (csi->marker == '?') {
    if (csi->final != 'u') return ParseSkip;

    parse->event->type = InputEventKeyboardReport;
    return ParseEvent;
  }

  if (csi->marker == '<' && csi->final == 'M') return Mouse(parse, csi, false);
  if (csi->marker == '<' && csi->final == 'm') return Mouse(parse, csi, true);
  if (csi->marker) return ParseSkip;

  switch (csi->final) {
    case 'u':
      return KittyKey(parse, first, modifiers);
    case '~':
      if (first == 200) return Paste(parse);
      return KeyEvent(parse, TildeKey(first), modifiers);
    case 'R':
      return CursorReport(parse, csi);
    default:
      return KeyEvent(parse, LetterKey(csi->final), modifiers);
  }
}

static enum ParseResult
ParseCsi(Parse *parse)
{
  // starts after the escape and '['.
  Csi csi = {};
  u64 index = 2;
  u32 field = 0;
  u32 part = 0;
  const u8 *bytes = (const u8 *)parse->text;

  if (index < parse->length && bytes[index] >= '<' && bytes[index] <= '?') {
    csi.marker = bytes[index++];
  }

  for (; index < parse->length; index++) {
    u8 byte = bytes[index];

    if (byte >= '0' && byte <= '9') {
      if (field >= 4 || part >= 3) continue;

      u32 *value = &csi.fields[field][part];
      if (*value < 1'000'000) *value = *value * 10 + (u32)(byte - '0');
    } else if (byte == ';') {
      field++;
      part = 0;
    } else if (byte == ':') {
      part++;
    } else if (byte >= 0x40 && byte <= 0x7E) {
      csi.final = byte;
      parse->size = index + 1;
      return InterpretCsi(parse, &csi);
    } else if (byte < 0x20 || byte > 0x2F) {
      // not a parameter, intermediate or final byte, so the sequence is malformed.
      parse->size = index;
      return ParseSkip;
    }
  }

  if (parse->length < maxSequenceLength) return ParseIncomplete;

  parse->size = parse->length;
  return ParseSkip;
}

static enum ParseResult
ParseControl(Parse *parse, u8 byte)
{
  // the only control bytes foot sends: enter, tab and backspace without modifiers.
  switch (byte) {
    case '\r':
      return KeyEvent(parse, KeyEnter, 0);
    case '\t':
      return KeyEvent(parse, KeyTab, 0);
    case 0x7F:
      return KeyEvent(parse, KeyBackspace, 0);
    default:
      return ParseSkip;
  }
}

static enum ParseResult
ParseText(Parse *parse)
{
  const u8 *bytes = (const u8 *)parse->text;

  if (bytes[0] < 0x20 || bytes[0] == 0x7F) {
    parse->size = 1;
    return ParseControl(parse, bytes[0]);
  }

  u32 expected = Utf8SequenceLength(bytes[0]);
  if (expected > parse->length) return ParseIncomplete;

  u32 codepoint;
  u32 size = Utf8Decode(parse->text, parse->length, &codepoint);
  parse->size = size;

  // a stray invalid byte is noise rather than something the user typed.
  if (codepoint == utf8Replacement && size == 1) return ParseSkip;
  return KeyEvent(parse, codepoint, 0);
}

// skips a control string (osc, dcs, apc, pm or sos) up to its end, bel or st.
static enum ParseResult
ParseString(Parse *parse)
{
  for (u64 at = 2; at < parse->length; at++) {
    bool bel = parse->text[at] == '\a';
    bool st = parse->text[at] == '\\' && parse->text[at - 1] == '\x1b';
    if (!bel && !st) continue;

    parse->size = at + 1;
    return ParseSkip;
  }

  return ParseIncomplete;
}

static enum ParseResult
ParseAny(Parse *parse)
{
  if (parse->text[0] != '\x1b') return ParseText(parse);
  if (parse->length < 2) return ParseIncomplete;

  // foot sends every key with an escape as a csi sequence, so anything else is noise,
  // skipped whole so none of it is typed: a string up to its end, ss3 with its final byte,
  // or else the escape and the byte after it.
  switch (parse->text[1]) {
    case '[':
      return ParseCsi(parse);
    case ']':
    case 'P':
    case '_':
    case '^':
    case 'X':
      return ParseString(parse);
    case 'O':
      if (parse->length < 3) return ParseIncomplete;
      parse->size = 3;
      return ParseSkip;
    default:
      parse->size = 2;
      return ParseSkip;
  }
}

void
InputFeed(const char *data, u64 length)
{
  // drop decoded bytes so the buffer only holds what is still pending.
  StrBufRemove(&input, 0, inputStart);
  inputStart = 0;
  StrBufAppend(&input, data, length);
}

bool
InputNext(InputEvent *event)
{
  bool expired = inputExpired;
  inputExpired = false;

  while (inputStart < input.length) {
    Parse parse =
      {.text = input.data + inputStart, .length = input.length - inputStart, .event = event};
    *event = (InputEvent){};
    enum ParseResult result = ParseAny(&parse);

    if (result == ParseIncomplete) {
      if (!expired || pasteOpen) return false;

      // the rest of the sequence is not coming, so it is dropped.
      parse.size = parse.length;
      result = ParseSkip;
    }

    inputStart += parse.size;
    if (result != ParseEvent) continue;

    event->raw = parse.text;
    event->rawLength = parse.size;
    return true;
  }

  return false;
}

bool
InputPending(void)
{
  return inputStart < input.length && !pasteOpen;
}

void
InputExpire(void)
{
  inputExpired = InputPending();
}
