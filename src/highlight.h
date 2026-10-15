#pragma once

// syntax highlighting with tree-sitter, and the only module that knows the grammars.
// a highlighter parses its buffer, parses it again incrementally after edits, and colours
// lines on demand by running the highlight queries over a window of lines around the one
// asked for.
// languages embedded in others (code blocks and inline text in markdown, scripts and
// styles in html) are parsed as separate layers over just their ranges.
#include "buffer.h"
#include "strbuf.h"
#include "theme.h"
#include "types.h"

typedef struct Highlight Highlight;

// a highlighter for a buffer in a language (by its display name, as in lang.c), or nullptr
// when there is no grammar for it.
// it follows the buffer's changes until HighlightFree.
// a grammar whose queries fail to compile is reported in message.
Highlight *
HighlightNew(Buffer *buffer, const char *language, StrBuf *message);

void
HighlightFree(Highlight *highlight);

// starts again from the text after it was replaced without telling listeners (a reload),
// and tries again after a parse that took too long.
void
HighlightReset(Highlight *highlight);

// true while the buffer is larger than maxHighlightSize, or since a parse took longer
// than maxParseTime, either of which turns highlighting off.
bool
HighlightDisabled(const Highlight *highlight);

// the group of every byte of a line (read with HighlightGroup), or nullptr when the
// buffer is not highlighted.
// a very long line is only coloured around focus, a byte near the part to be drawn.
// valid until the buffer changes or a line far from this one is asked for.
const u8 *
HighlightLine(Highlight *highlight, u64 line, u64 focus);

static inline enum ThemeGroup
HighlightGroup(const u8 *groups, u64 byte)
{
  return (enum ThemeGroup)(groups[byte] & 0x3F);
}

// true when spell check looks at a byte: anything outside code in a prose language
// (markdown), and only comments and strings in code.
bool
HighlightSpellable(Highlight *highlight, u64 line, u64 byte);
