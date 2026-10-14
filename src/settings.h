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

// syntax highlighting is skipped for buffers larger than this.
static constexpr u64 maxHighlightSize = 16ull * 1024 * 1024;

// a split view needs at least this many columns.
static constexpr i32 minSplitWidth = 80;

// smallest window the editor will draw into.
static constexpr i32 minWindowWidth = 20;
static constexpr i32 minWindowHeight = 5;
