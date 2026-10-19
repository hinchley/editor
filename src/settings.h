#pragma once

// settings baked into the editor.
// values marked "default" can be toggled at runtime and only set the initial state.
#include "types.h"

enum CursorStyle {
  CursorStyleLine,
  CursorStyleBlock
};

// width of a tab stop, in columns.
static constexpr i32 tabWidth = 2;

// wrap long lines to the window width.
static constexpr bool softWrapDefault = false;

// rows and columns kept between the cursor and the window edge.
static constexpr i32 scrollOffset = 4;

// show the line number gutter.
static constexpr bool lineNumbersDefault = true;

// shape and blink of the terminal cursor.
static constexpr enum CursorStyle cursorStyle = CursorStyleLine;
static constexpr bool cursorBlink = true;

// render spaces and tabs as faint symbols.
static constexpr bool showWhitespaceDefault = true;

// vertical guide drawn just after rulerColumn.
static constexpr bool rulerDefault = false;
static constexpr i32 rulerColumnDefault = 100;
static constexpr i32 rulerColumnMinimum = 1;
static constexpr i32 rulerColumnMaximum = 500;

// insert spaces up to the next tab stop instead of a tab character.
static constexpr bool expandTabDefault = true;

// strip trailing whitespace from every line when saving.
static constexpr bool trimTrailingWhitespace = false;

// most files that can be loaded at once.
static constexpr u64 maxFileCount = 100'000;

// files larger than this are loaded but never opened.
static constexpr u64 maxFileSize = 100ull * 1024 * 1024;

// a nul byte within this many leading bytes marks a file as binary.
static constexpr u64 binaryProbeSize = 4096;

// project search lists at most this many matching lines.
static constexpr u64 maxProjectHits = 10'000;

// syntax highlighting is skipped for buffers larger than this.
static constexpr u64 maxHighlightSize = 16ull * 1024 * 1024;

// a parse that takes longer than this many milliseconds turns highlighting off for the
// buffer, rather than leave the editor waiting.
static constexpr i64 maxParseTime = 1000;

// a symbol's signature is cut to this many bytes.
static constexpr u64 maxSignatureLength = 512;

// how many places jump history (alt+[, alt+]) keeps.
static constexpr u32 jumpHistorySize = 32;

// a split view needs at least this many columns.
static constexpr i32 minSplitWidth = 80;

// smallest window the editor will draw into.
static constexpr i32 minWindowWidth = 20;
static constexpr i32 minWindowHeight = 5;

// change marks against the last git commit are left out for buffers larger than this.
static constexpr u64 maxDiffSize = 16ull * 1024 * 1024;

// the most line edits a diff looks for in one part before it reports the part as one
// change, which keeps comparing unrelated texts quick.
static constexpr u64 maxDiffCost = 1024;

// start the language server a language's package names when a file of it opens.
static constexpr bool languageServersDefault = true;

// buffers larger than this are not shown to a language server.
static constexpr u64 maxServerFileSize = 16ull * 1024 * 1024;

// a document's changes are sent to its language server as they were made, unless more
// than this many pile up between two passes of the main loop (many cursors, replace all),
// when the whole text is sent instead.
static constexpr u32 maxServerChanges = 64;

// how many milliseconds language servers have to exit once asked to, before they are
// ended.
static constexpr i64 serverShutdownTime = 500;

// how many milliseconds the cursor rests before the diagnostics panel asks the language
// server what the symbol under it is.
static constexpr i64 hoverDelay = 150;

// how many milliseconds a language server has to answer a command (go to definition,
// references) before the command gives up on it.
static constexpr i64 serverRequestTimeout = 5000;
