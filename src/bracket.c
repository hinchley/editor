#include "bracket.h"

static constexpr char openers[] = "([{";
static constexpr char closers[] = ")]}";
static constexpr u32 kindCount = 3;

// the kind of bracket a byte is (0 to 2), or kindCount when it is not one.
static u32
KindOf(char byte, const char *brackets)
{
  for (u32 kind = 0; kind < kindCount; kind++) {
    if (brackets[kind] == byte) return kind;
  }

  return kindCount;
}

// searches forward from just after an opening bracket for the closer of the same kind.
// brackets are ascii, so stepping over bytes never splits a match out of a utf-8 sequence.
static bool
FindCloser(const Buffer *buffer, Position from, u32 kind, Position *match)
{
  u64 depth = 0;
  u64 lineCount = BufferLineCount(buffer);

  for (u64 line = from.line; line < lineCount; line++) {
    const StrBuf *text = BufferLine(buffer, line);

    for (u64 byte = line == from.line ? from.byte : 0; byte < text->length; byte++) {
      char character = text->data[byte];

      if (character == openers[kind]) {
        depth++;
      } else if (character == closers[kind]) {
        if (depth == 0) {
          *match = (Position){line, byte};
          return true;
        }

        depth--;
      }
    }
  }

  return false;
}

// searches backward from just before a closing bracket for the opener of the same kind.
static bool
FindOpener(const Buffer *buffer, Position from, u32 kind, Position *match)
{
  u64 depth = 0;

  for (u64 line = from.line + 1; line-- > 0;) {
    const StrBuf *text = BufferLine(buffer, line);

    for (u64 byte = line == from.line ? from.byte : text->length; byte-- > 0;) {
      char character = text->data[byte];

      if (character == closers[kind]) {
        depth++;
      } else if (character == openers[kind]) {
        if (depth == 0) {
          *match = (Position){line, byte};
          return true;
        }

        depth--;
      }
    }
  }

  return false;
}

bool
BracketMatch(const Buffer *buffer, Position position, Position *match)
{
  const StrBuf *text = BufferLine(buffer, position.line);
  if (position.byte >= text->length) return false;

  char character = text->data[position.byte];
  u32 kind = KindOf(character, openers);

  if (kind < kindCount) {
    Position from = {position.line, position.byte + 1};
    return FindCloser(buffer, from, kind, match);
  }

  kind = KindOf(character, closers);
  return kind < kindCount && FindOpener(buffer, position, kind, match);
}

// checks an unmatched opening bracket found before range.
static bool
Encloses(const Buffer *buffer, Position opener, u32 kind, Range range, Range *pair)
{
  Position closer;
  Position from = {opener.line, opener.byte + 1};
  if (!FindCloser(buffer, from, kind, &closer)) return false;
  if (PositionCompare(closer, range.end) < 0) return false;

  *pair = (Range){opener, closer};
  return true;
}

bool
BracketEnclosing(const Buffer *buffer, Range range, Range *pair)
{
  // closing brackets passed on the way back are waiting for their openers.
  u64 waiting[kindCount] = {};
  Position from = range.start;

  for (u64 line = from.line + 1; line-- > 0;) {
    const StrBuf *text = BufferLine(buffer, line);

    for (u64 byte = line == from.line ? from.byte : text->length; byte-- > 0;) {
      char character = text->data[byte];
      u32 kind = KindOf(character, closers);

      if (kind < kindCount) {
        waiting[kind]++;
        continue;
      }

      kind = KindOf(character, openers);
      if (kind == kindCount) continue;

      if (waiting[kind]) {
        waiting[kind]--;
        continue;
      }

      if (Encloses(buffer, (Position){line, byte}, kind, range, pair)) return true;
    }
  }

  return false;
}
