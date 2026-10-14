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

// a keypad key the kitty protocol reports with a private use code point.
typedef struct KeypadKey {
  u32 code;
  u32 key;
} KeypadKey;

static const KeypadKey keypadKeys[] = {
  {57399, '0'}, {57400, '1'}, {57401, '2'}, {57402, '3'}, {57403, '4'}, {57404, '5'},
  {57405, '6'}, {57406, '7'}, {57407, '8'}, {57408, '9'}, {57409, '.'}, {57410, '/'},
  {57411, '*'}, {57412, '-'}, {57413, '+'}, {57414, KeyEnter}, {57415, '='}, {57416, ','},
  {57417, KeyLeft}, {57418, KeyRight}, {57419, KeyUp}, {57420, KeyDown},
  {57421, KeyPageUp}, {57422, KeyPageDown}, {57423, KeyHome}, {57424, KeyEnd},
  {57425, KeyInsert}, {57426, KeyDelete}
};

static constexpr char pasteEnd[] = "\x1b[201~";
static constexpr u64 pasteEndLength = sizeof pasteEnd - 1;

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
  return (u8)((value - 1) & (KeyModifierShift | KeyModifierAlt | KeyModifierControl |
    KeyModifierSuper));
}

static enum ParseResult
KeyEvent(Parse *parse, u32 code, u8 modifiers)
{
  if (code == 0) return ParseSkip;

  parse->event->type = InputEventKey;
  parse->event->key = (Key){code, modifiers};
  return ParseEvent;
}

static enum ParseResult
KittyKey(Parse *parse, u32 code, u8 modifiers)
{
  for (u32 index = 0; index < sizeof keypadKeys / sizeof keypadKeys[0]; index++) {
    const KeypadKey *keypad = &keypadKeys[index];
    if (keypad->code == code) return KeyEvent(parse, keypad->key, modifiers);
  }

  // other private use codes are functional keys the editor has no use for.
  if (code >= 57344 && code <= 63743) return ParseSkip;

  // the protocol reports the unshifted key, but be tolerant of terminals that do not.
  if (code >= 'A' && code <= 'Z') {
    code += 'a' - 'A';
    modifiers |= KeyModifierShift;
  }

  // shift alone on a letter is just a capital letter.
  if (modifiers == KeyModifierShift && code >= 'a' && code <= 'z') {
    code -= 'a' - 'A';
    modifiers = 0;
  }

  return KeyEvent(parse, code, modifiers);
}

static u32
TildeKey(u32 number)
{
  switch (number) {
  case 1:
  case 7:
    return KeyHome;
  case 2:
    return KeyInsert;
  case 3:
    return KeyDelete;
  case 4:
  case 8:
    return KeyEnd;
  case 5:
    return KeyPageUp;
  case 6:
    return KeyPageDown;
  default:
    return 0;
  }
}

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
    if (csi->final == 'R') return CursorReport(parse, csi);
    if (csi->final != 'u') return ParseSkip;

    parse->event->type = InputEventKeyboardReport;
    parse->event->flags = first;
    return ParseEvent;
  }

  if (csi->marker) return ParseSkip;

  switch (csi->final) {
  case 'u':
    return KittyKey(parse, first, modifiers);
  case '~':
    if (first == 200) return Paste(parse);
    return KeyEvent(parse, TildeKey(first), modifiers);
  case 'Z':
    return KeyEvent(parse, KeyTab, KeyModifierShift);
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
ParseControl(Parse *parse, u8 byte, u8 modifiers)
{
  // legacy control bytes, still sent for unmodified enter, tab and backspace.
  switch (byte) {
  case '\r':
    return KeyEvent(parse, KeyEnter, modifiers);
  case '\t':
    return KeyEvent(parse, KeyTab, modifiers);
  case 0x08:
  case 0x7F:
    return KeyEvent(parse, KeyBackspace, modifiers);
  case 0x00:
    return KeyEvent(parse, KeySpace, modifiers | KeyModifierControl);
  default:
    if (byte > 0x1A) return ParseSkip;
    return KeyEvent(parse, (u32)('a' + byte - 1), modifiers | KeyModifierControl);
  }
}

static enum ParseResult
ParseText(Parse *parse, u64 offset, u8 modifiers)
{
  const u8 *bytes = (const u8 *)parse->text + offset;
  u64 length = parse->length - offset;

  if (bytes[0] < 0x20 || bytes[0] == 0x7F) {
    parse->size = offset + 1;
    return ParseControl(parse, bytes[0], modifiers);
  }

  u32 expected = Utf8SequenceLength(bytes[0]);
  if (expected > length) return ParseIncomplete;

  u32 codepoint;
  u32 size = Utf8Decode((const char *)bytes, length, &codepoint);
  parse->size = offset + size;

  // a stray invalid byte is noise rather than something the user typed.
  if (codepoint == utf8Replacement && size == 1) return ParseSkip;
  return KittyKey(parse, codepoint, modifiers);
}

static enum ParseResult
ParseEscape(Parse *parse)
{
  if (parse->length < 2) return ParseIncomplete;

  u8 next = (u8)parse->text[1];
  if (next == '[') return ParseCsi(parse);

  if (next == 'O') {
    // ss3 form of the cursor keys.
    if (parse->length < 3) return ParseIncomplete;

    parse->size = 3;
    return KeyEvent(parse, LetterKey((u8)parse->text[2]), 0);
  }

  if (next == 0x1B) {
    parse->size = 1;
    return KeyEvent(parse, KeyEscape, 0);
  }

  // legacy alt prefix.
  return ParseText(parse, 1, KeyModifierAlt);
}

static enum ParseResult
ParseAny(Parse *parse)
{
  if (parse->text[0] == '\x1b') return ParseEscape(parse);
  return ParseText(parse, 0, 0);
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
    Parse parse = {
      .text = input.data + inputStart,
      .length = input.length - inputStart,
      .event = event
    };
    *event = (InputEvent){};
    enum ParseResult result = ParseAny(&parse);

    if (result == ParseIncomplete) {
      if (!expired || pasteOpen) return false;

      // a lone escape was the key itself, anything longer is a broken sequence.
      parse.size = parse.length;
      result = parse.length == 1 ? KeyEvent(&parse, KeyEscape, 0) : ParseSkip;
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
