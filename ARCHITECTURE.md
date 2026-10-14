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
| `utf8` | UTF-8 decode and encode, grapheme cluster boundaries, display width and simple case folding, from the Unicode tables. |
| `buffer` | The text of one buffer as a flat array of lines, and `BufferReplace`, the single edit primitive, which notifies listeners. |
| `file` | Loading a file into a buffer (size and binary checks, line ending detection) and saving it atomically. |
| `term` | The terminal session: raw mode, startup handshake, alternate screen, keyboard protocol, bracketed paste, cursor shape, window size, waiting for input or a resize, and restoring the terminal on every exit path. |
| `input` | Decodes the bytes read from the terminal into events: keys, pastes, cursor position reports and keyboard protocol reports. Pure parsing, no I/O. |
| `key` | The `Key` type (code plus modifiers) and key names such as `ctrl+shift+up`. |
| `keymap` | The binding table: key, command and help text. The single source of truth for dispatch and the Alt-H list. |
| `screen` | A grid of styled cells drawn in full each frame. Flushing writes only the cells that changed, inside a synchronized update. |
| `theme` | Highlight groups (syntax and interface) and the style each one draws with. |
| `layout` | How one line sits on screen: grapheme clusters with their display columns and widths (tabs expanded), and the visual rows it wraps into. |
| `cursor` | The cursor set of a view: anchor, head and desired column per cursor, one of them primary. |
| `view` | A window onto a buffer: screen area, gutter, cursors, scroll position, stepping over visual rows and keeping the cursor in sight. |
| `move` | Cursor movement commands, written once against visual rows. |
| `render` | Draws a frame: the view (gutter, text, whitespace symbols, ruler), the status bar and the message bar. |

Later milestones add `undo`, `edit`, `prompt`, `picker`, `loader`, `filter`, `search`, `spell`, `clipboard`, `lang`, `highlight` and `project`.

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

## Text

### Buffer

A buffer is a flat array of lines. Each line is a `StrBuf` holding its bytes without the line break, so joining the lines with `\n` gives the text. There is always at least one line: an empty buffer is one empty line, and text ending in `\n` has a final empty line. A `Position` is a line index plus a byte offset in that line. Display columns are always derived from the bytes, never stored.

Every change is `BufferReplace(range, text)`: insertion is an empty range, deletion is empty text. After each replace the buffer calls its listeners with a `BufferChange` (start, old end, new end, old text, new text). Cursors, views, highlighting and undo all follow edits through listeners, so there is one place where text changes. Undo and redo are inverse replaces. A listener must not change the buffer while it is being notified. Edits inside one line take a fast path that touches only that line. `BufferSetText` replaces everything without notifying, for loading.

### Text and Unicode

Bytes are stored exactly as read. An invalid byte is treated as one U+FFFD code point wherever text is decoded, so invalid UTF-8 survives editing and saving untouched. Cursor movement steps over grapheme clusters, using the extended grapheme cluster rules except the Indic conjunct rule (GB9c), which would need another table. `Utf8PreviousBoundary` works backwards from the cursor with only the context it needs (a run of regional indicators, or an emoji ZWJ sequence), so moving left on a very long line stays cheap. A cluster is as wide as its first code point, except that a regional indicator pair (a flag) and a pictographic character followed by U+FE0F are two columns.

### Files

`FileLoad` refuses anything that is not a regular file, files over `maxFileSize` (100 MB) and files with a nul byte in the first `binaryProbeSize` (4 KB) bytes. A missing file is not an error: it gives an empty buffer that the first save creates. The file is opened with `O_NONBLOCK` so a named pipe is refused instead of hanging. Line endings are normalised in one pass: every `\r\n` becomes `\n`, and the more common of the two endings (LF on a tie) is recorded and written back on save. A file with both endings is normalised to the more common one, with a message. A `\r` that is not before `\n` is ordinary content (shown as `^M`). So a file that uses one ending consistently round trips byte for byte.

`FileSave` writes to a temporary file (`.<name>.editor-XXXXXX`) in the target's folder, sets its permissions, `fsync`s it and renames it over the target. Symbolic links are resolved first, so the link stays and its final target is replaced. A broken link is refused, because the rename would replace the link with a regular file. The permissions of an existing file are kept, and a new file gets `0666` less the umask. Ownership is not copied, and a hard linked file loses its link, because the rename gives it a new inode.

Each `File` keeps a `FileStamp` (device, inode, size, modification time) from its last load or save. Before writing, the target is checked against it. If the file changed on disk, or appeared where there was none, the save returns `FileStatusChanged` and is only done when the caller asks again with `overwriteChanged`. A file deleted since loading is simply written again. An existing file without write permission is refused, even though the rename would succeed, because the folder is writable.

## Views

### Layout

`layout` is the only place that turns bytes into columns. `LayoutNext` walks a line one grapheme cluster at a time and gives each cluster its display column (from the line start) and width. Printable ASCII followed by ASCII takes a fast path that skips the grapheme rules. A tab is as wide as the distance to the next multiple of `tabWidth`, measured from the start of the logical line even when the line is wrapped. An ASCII control character is drawn as `^X` in two columns. An invalid byte, a C1 control character, or a cluster with no width of its own (such as a lone U+200B) is drawn as U+FFFD in one column, so nothing on a line is invisible.

`LayoutWrap` splits a line into visual rows. A row breaks after its last space or tab that fits, or else just before the cluster that does not fit. Wrapped rows have no continuation indent. When the last row is exactly full, an empty row follows it, so the cursor at the end of the line has a cell to sit in. Without soft wrap a line is one row.

### Visual rows

A `ViewRow` is a line plus the index of one of its rows. `ViewStep` moves forward or back by any number of visual rows and stops at the ends of the buffer. Vertical movement, paging, scrolling and the scroll margin are all written against `ViewStep`, so the same code serves wrapped and unwrapped views. Nothing holds a row index for the whole buffer. A wrapped line is laid out when it is reached, so the cost depends on the rows stepped over, not the buffer size. Without soft wrap every line is one row, and stepping is plain arithmetic on line numbers, so a jump across a million lines is instant.

Each cursor keeps a desired column: its column within its visual row. Up, Down, PageUp and PageDown aim for it, and every other movement resets it. Home and End act on the logical line. Up on the first row goes to the start of the buffer, and Down on the last row to the end.

### Scrolling

The view keeps its first visible row (`top`) and, without soft wrap, its first visible column (`leftColumn`). `ViewScrollToCursor` runs before every frame, so movement, resizing and toggles all end with the primary cursor in sight. It keeps `scrollOffset` rows between the cursor and the top and bottom edges (capped at just under half the height). The margin shrinks to the rows that exist before or after the cursor, which is how it yields at the start and end of the buffer. The view never scrolls so far that empty rows show below the end of the buffer. Columns get the same margin, and it yields at the start of the line.

Ctrl-Up and Ctrl-Down move `top` by one row and leave the cursor alone. They refuse (with a message) when the cursor would end up inside the margin, or the view would pass either end. PageUp and PageDown move both the cursor and `top` by the height less one row, so the cursor keeps its screen row.

### Rendering

`RenderFrame` gives the view every row except the bottom two (the status bar and the message bar), scrolls it to its cursor and draws it. The gutter is the line number right aligned with a space either side, at least three digits wide. Only the first row of a wrapped line is numbered. With whitespace display on, a space is drawn as `·` and a tab as `→` in its first cell, in the `whitespace` colour. The ruler is drawn as `│` in the cell just after `rulerColumn` before the text, so text covers it, and spaces and tabs let it show through. A cluster cut by the left or right edge of the text area shows as blank cells.

The status bar shows the name (shortened in the middle to fit) with `[+]` and `[RO]` on the left, and the wrap state, file type, line ending, `line:column`, line count and `open:loaded` buffer counts on the right. When the window is too narrow it keeps only `line:column`, and then nothing, on the right. Below `minWindowWidth` by `minWindowHeight` the editor shows only "terminal too small".

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
