# Architecture

This document describes how the editor is put together. It grows with each milestone.

## Layout

| Path | Contents |
|---|---|
| `src/` | All editor sources, flat, `.c` and `.h` together. |
| `vendor/tree-sitter/` | Tree-sitter runtime (`include/`, `src/`). Compiled through its `src/lib.c` amalgamation. |
| `vendor/grammars/<lang>/` | Generated grammar sources (`parser.c`, optional `scanner.c`) and their `tree_sitter/` headers. |
| `vendor/queries/<lang>/` | `highlights.scm` and, where the grammar embeds other languages, `injections.scm`. |
| `data/words.txt` | Spell check word list, one word per line, sorted in byte order (`LC_ALL=C`). |
| `tools/` | Developer scripts that are not part of the binary. |
| `build.sh` | The build. Outputs `./editor`, objects go to `build/`. |

## Glossary

- **Loaded**: a file is in the buffer list.
- **Open**: a loaded buffer whose contents have been read into memory. Files from folders are loaded at startup but only opened when first used.
- **Display column**: a 1-based screen column within a line, with tabs expanded and wide characters counted as two. Used by the status bar, go to line and the ruler.

## Modules

| Module | Responsibility |
|---|---|
| `main` | Calls `AppRun`. Nothing else. |
| `app` | Startup and the main loop. |
| `types.h` | Fixed width numeric aliases (`u8`..`u64`, `i8`..`i64`, `f32`, `f64`). The only numeric types used. |
| `settings.h` | Built-in settings as named `constexpr` values. Values with a `Default` suffix only set the initial state of a runtime toggle. |
| `fatal` | `Fatal` restores the terminal (through a registered cleanup hook), prints a message and exits. |
| `memory` | Checked allocation. Every allocation either succeeds or ends in `Fatal`, so callers never test for null. Memory is released with `free`. |
| `strbuf` | Growable byte string. A zeroed `StrBuf` is empty and valid. Allocated data is always nul terminated. |
| `unicodedata` | Generated Unicode tables: display width, grapheme break property, Extended_Pictographic and simple case folding. |
| `utf8` | UTF-8 decode and encode, and code point display width from the Unicode tables. |
| `term` | The terminal session: raw mode, startup handshake, alternate screen, keyboard protocol, bracketed paste, cursor shape, window size, waiting for input or a resize, and restoring the terminal on every exit path. |
| `input` | Decodes the bytes read from the terminal into events: keys, pastes, cursor position reports and keyboard protocol reports. Pure parsing, no I/O. |
| `key` | The `Key` type (code plus modifiers) and key names such as `ctrl+shift+up`. |
| `keymap` | The binding table: key, command and help text. The single source of truth for dispatch and the Alt-H list. |
| `screen` | A grid of styled cells drawn in full each frame. Flushing writes only the cells that changed, inside a synchronized update. |
| `keycheck` | Temporary (M1 to M3). A screen that names every key and tracks which bindings have been seen, used to verify input in each terminal. |

Later milestones add `render`, `buffer`, `file`, `undo`, `cursor`, `layout`, `view`, `edit`, `move`, `prompt`, `picker`, `loader`, `filter`, `search`, `spell`, `clipboard`, `lang`, `highlight`, `theme` and `project`.

## Terminal

Startup (`TermStart`):

1. Check that standard input and output are a terminal, save the terminal mode, install signal handlers and register `TermStop` as the `Fatal` cleanup.
2. Enter raw mode with `cfmakeraw`.
3. Handshake: send the keyboard protocol query (`CSI ? u`) followed by a cursor position request (`CSI 6 n`). Every terminal answers the second, so a cursor report with no keyboard report before it means the kitty keyboard protocol is missing, and the editor exits with an error. No answer within a second is also an error. This happens before the alternate screen is entered, so a failure leaves nothing on screen.
4. Enter the alternate screen, push keyboard protocol flags (disambiguate only), enable bracketed paste and set the cursor shape.

`TermStop` undoes these in reverse order. The keyboard protocol keeps a separate flag stack per screen, so the flags are popped before leaving the alternate screen. `TermStop` only uses `write` and `tcsetattr`, so it is safe in a signal handler. It runs on normal exit, through `Fatal`, and from handlers for SIGTERM, SIGHUP, SIGINT, SIGQUIT, SIGSEGV, SIGBUS, SIGFPE, SIGILL and SIGABRT. Those handlers then re-raise the signal with its default action.

SIGWINCH writes a byte to a self-pipe. The main loop `poll`s standard input and the pipe, so a resize wakes it like input does.

### Input

With only the disambiguate flag, plain text (including shifted characters) still arrives as UTF-8, and unmodified Enter, Tab and Backspace still send their legacy bytes. Every key with Ctrl or Alt, and Escape, arrives as `CSI code ; modifiers u`. Terminals still use the legacy forms for some keys under the protocol (`CSI 1;5A`, `CSI 5~`, `CSI H`), so the decoder accepts both. Key codes are always the unshifted key, so Ctrl-Shift-S is `{'s', ctrl|shift}`. Lock modifiers are ignored.

Escape is a sequence under the protocol, so a lone escape byte only appears when a sequence is split across reads. The main loop waits up to 50 ms for the rest, then treats it as the Escape key. A bracketed paste is one event however many reads it spans, and its text is passed on unchanged.

### Screen

Each frame clears the back grid, draws everything, and `ScreenFlush` compares it with the previous frame. A cell holds a whole grapheme cluster (its bytes live in a per-frame text buffer) and a style. A wide character uses two cells, the second with width 0. After writing any non-ASCII cell the writer moves the cursor explicitly before the next cell, so a terminal that disagrees about a character's width cannot shift the rest of the row. A resize clears the terminal and redraws everything.

## Conventions

- Types are `PascalCase` and structs are typedef'd (`StrBuf`). Enum constants are `PascalCase` prefixed with their enum name (`CursorStyleLine`, `GraphemeBreakExtend`).
- Functions are `PascalCase` and prefixed with their module name (`StrBufAppend`, `MemoryResize`). Variables and constants are `camelCase`.
- Headers use `#pragma once`.
- C23 features in use: `bool`, `nullptr`, `constexpr`, `[[attributes]]`, `#embed`, `<stdckdint.h>`, digit separators.
- Data embedded with `#embed` goes into `u8` arrays, not `char`. A `char` array trips `-Wconversion` on every byte above 127. Embed paths are quoted and relative to the including file (`#embed "../data/words.txt"`), so they work without extra flags in gcc and clangd.
- Vendored code is compiled with `-w` and never edited. Strict warnings (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror`) apply to `src/` only.

## Vendored versions

| Component | Source | Version | ABI |
|---|---|---|---|
| tree-sitter runtime | github.com/tree-sitter/tree-sitter | v0.27.1 | 13 to 15 |
| C | github.com/tree-sitter/tree-sitter-c | v0.24.2 | 15 |
| HTML | github.com/tree-sitter/tree-sitter-html | v0.23.2 | 14 |
| CSS | github.com/tree-sitter/tree-sitter-css | v0.25.0 | 15 |
| JSON | github.com/tree-sitter/tree-sitter-json | v0.24.8 | 14 |
| JavaScript | github.com/tree-sitter/tree-sitter-javascript | v0.25.0 | 15 |
| Markdown (block and inline) | github.com/tree-sitter-grammars/tree-sitter-markdown | v0.5.3 | 15 |

Each vendored folder keeps its upstream `LICENSE`. The runtime's WebAssembly support (`wasm-stdlib`) is left out. `wasm_store.c` is kept because `lib.c` includes it, but it compiles to nothing without `TREE_SITTER_FEATURE_WASM`.

Notes for the highlighting milestone:

- The runtime does not evaluate text predicates (`#match?`, `#eq?`, `#any-of?`). For example, the C query captures `main` as `constant` until `#match? "^[A-Z]..."` is applied. `highlight.c` must filter matches itself.
- The Markdown queries capture `text.title`, `text.literal`, `text.emphasis`, `text.strong`, `text.uri` and `text.reference`. With the first-component rule these would all share one colour, so the theme needs a way to tell them apart.
- The Markdown block grammar injects the inline grammar under the id `markdown_inline`.

## Unicode tables

`src/unicodedata.c` is generated and committed. To regenerate it, for example for a new Unicode version:

1. Download `UnicodeData.txt`, `EastAsianWidth.txt`, `CaseFolding.txt`, `auxiliary/GraphemeBreakProperty.txt` and `emoji/emoji-data.txt` from `https://www.unicode.org/Public/<version>/ucd/` into one folder.
2. Run `tools/unicodedata.py <folder> > src/unicodedata.c`.

The current tables are Unicode 16.0.0. Width rules: East Asian Wide and Fullwidth are 2 columns. Nonspacing marks, enclosing marks, format characters (except the soft hyphen), Hangul medial vowels and final consonants, and U+200B are 0. Everything else is 1.
