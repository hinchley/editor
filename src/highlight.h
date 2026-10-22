#pragma once

// syntax highlighting with tree-sitter, and the only module that loads the grammars (from
// the language packages lang.c reads).
// a highlighter parses its buffer, parses it again incrementally after edits, and colours
// lines on demand by running the highlight queries over a window of lines around the one
// asked for.
// languages embedded in others (code blocks and inline text in markdown, scripts and
// styles in html) are parsed as separate layers over just their ranges.
#include "buffer.h"
#include "lang.h"
#include "strbuf.h"
#include "theme.h"
#include "types.h"

typedef struct Highlight Highlight;

// a highlighter for a buffer in a language, or nullptr when it has no grammar.
// it follows the buffer's changes until HighlightFree.
// a grammar that fails to load, or whose queries fail to compile, is reported in message
// the first time.
Highlight *
HighlightNew(Buffer *buffer, const Lang *lang, StrBuf *message);

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

// one symbol a grammar's tags query finds (see ARCHITECTURE.md, "Symbols").
// the texts are only valid while the visitor runs.
typedef struct HighlightSymbol {
  // what the symbol is: "function", "prototype", "type", "struct", "enum", "macro" or
  // "heading".
  const char *kind;

  // where its definition starts, and where its name is.
  Position start;
  Range name;

  const char *nameText;
  u64 nameLength;

  // the definition up to its body, without comments and with blanks collapsed, cut to
  // maxSignatureLength.
  const char *signature;
  u64 signatureLength;
} HighlightSymbol;

typedef void (*HighlightVisit)(void *context, const HighlightSymbol *symbol);

// true when a language has symbols.
bool
HighlightHasSymbols(const Lang *lang);

// visits the symbols of the buffer in order of their definitions, parsing it first if it
// changed.
// returns false, visiting nothing, when its language has no symbols or the buffer is not
// highlighted (see HighlightDisabled).
bool
HighlightSymbols(Highlight *highlight, HighlightVisit visit, void *context);

// visits the symbols of text in a language, as HighlightSymbols does, parsing it with a
// parser of its own within maxParseTime.
// returns false only when the parse ran out of time, which may not happen again; a
// language without symbols, or text too large to parse, visits nothing.
bool
HighlightSymbolsOfText(
  const Lang *lang,
  const char *text,
  u64 length,
  HighlightVisit visit,
  void *context
);

// the smallest syntax node that holds range and more, in the buffer's language or one
// embedded in it.
// returns false when there is none or the buffer is not highlighted.
bool
HighlightNodeAround(Highlight *highlight, Range range, Range *outer);

// the range of the smallest node of a type (the grammar's name for it, such as
// "pipe_table") that holds position, in the buffer's own language.
// returns false when there is none or the buffer is not highlighted.
bool
HighlightNodeOfType(Highlight *highlight, Position position, const char *type, Range *range);
