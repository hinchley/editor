# Architecture

This document describes how the editor is put together.

## Layout

| Path | Contents |
|---|---|
| `src/` | All editor sources, flat, `.c` and `.h` together. |
| `vendor/tree-sitter/` | Tree-sitter runtime (`include/`, `src/`). Compiled through its `src/lib.c` amalgamation. |
| `vendor/grammars/<lang>/` | Generated grammar sources (`parser.c`, optional `scanner.c`) and their `tree_sitter/` headers. |
| `vendor/queries/<lang>/` | `highlights.scm` and, where the grammar embeds other languages, `injections.scm`. |
| `data/words.txt` | Spell check word list, one word per line, sorted in byte order (`LC_ALL=C`). |
| `tools/` | Developer scripts that are not part of the binary: `unicodedata.py` generates `src/unicodedata.c`, and `bindings.py` writes the key binding tables in README.md from `src/keymap.c`. |
| `build.sh` | The build. Outputs `./editor`, objects go to `build/`. |

## Glossary

- **Loaded**: a file is in the buffer list.
- **Open**: a loaded buffer whose contents have been read into memory. Files from folders are loaded at startup but only opened when first used.
- **Display column**: a 1-based screen column within a line, with tabs expanded and wide characters counted as two. Used by the status bar, go to line and the ruler.

## Data flow

The editor is one thread around one loop in `app`:

1. **Input.** `TermWait` polls standard input and the resize pipe. `input` decodes the bytes into events: keys, pastes and terminal replies.
2. **Dispatch.** A running project search, an open picker, the find and replace prompts and any other prompt take a key first, in that order. Otherwise a printable key is typed, and any other key is looked up in `keymap`, whose command `app` runs.
3. **Commands** live in the modules that own what they change (`move`, `edit`, `search`, `session`, `save`, `project`). They work on the current document's focused view through `workspace`, and change text only through `BufferReplace`.
4. **Listeners.** Every replace notifies the buffer's listeners: both views of the document (cursors and scroll position), its undo history (records) and its highlighter (tree edits). Nothing else needs telling, so a command does not know who shows the text.
5. **Frame.** Once no input is waiting, `RenderFrame` scrolls each view to its cursors and draws the views, status bars and message bar into the `screen` grid, laying out only the visible rows and colouring only the visible lines. `ScreenFlush` writes the cells that changed.

Loading runs before the loop: `loader` turns the command line into the list of files, `workspace` holds them as entries, and an entry's file is read into a `document` the first time it is used.

## Modules

| Module | Responsibility |
|---|---|
| `main` | Calls `AppRun`. Nothing else. |
| `app` | Startup, the main loop, key dispatch, the Alt-H list, the commands on the current document that ask questions (reload, go to line, ruler column, sort order), and the ones that report on it (statistics, copy path). Statistics counts on demand: words as runs of `Utf8IsWord` code points, characters as grapheme clusters (`Utf8NextBoundary`), and bytes with line breaks as the file's line ending; 15 MB counts in about 0.5 s. |
| `types.h` | Fixed width numeric aliases (`u8`..`u64`, `i8`..`i64`, `f32`, `f64`). The only numeric types used. |
| `settings.h` | Built-in settings as named `constexpr` values. Values with a `Default` suffix only set the initial state of a runtime toggle. |
| `fatal` | `Fatal` restores the terminal (through a registered cleanup hook), prints a message and exits. |
| `memory` | Checked allocation. Every allocation either succeeds or ends in `Fatal`, so callers never test for null. Memory is released with `free`. |
| `strbuf` | Growable byte string. A zeroed `StrBuf` is empty and valid. Allocated data is always nul terminated. |
| `unicodedata` | Generated Unicode tables: display width, grapheme break property, Extended_Pictographic and simple case folding. |
| `utf8` | UTF-8 decode and encode, grapheme cluster boundaries, display width and simple case folding, from the Unicode tables. |
| `buffer` | The text of one buffer as a flat array of lines, and `BufferReplace`, the single edit primitive, which notifies listeners. |
| `file` | Loading a file into a buffer (size and binary checks, line ending detection) and saving it atomically. |
| `undo` | The undo history of one buffer: groups of recorded changes with the cursors before and after each. |
| `document` | A buffer with its file, undo history and a view for each side of a split, whether it differs from the file, what it highlights (search matches, unknown words or nothing), and its language and highlighter. |
| `filter` | Path patterns from `.editor` files: names, folders, anchored paths and `*`. |
| `loader` | Turns the paths from the command line (or Ctrl-O) into the list of files to load: walking folders, applying `.editor` files, following file links, removing duplicates and enforcing the file limit. |
| `workspace` | The buffer list: loaded entries, which are open, the current one, and when each was last used. Opens an entry the first time it is used. Also the split: whether there are two views, what the other one shows and which side is which. |
| `session` | Commands that change which buffers are loaded or current: startup loading, Ctrl-N, Ctrl-O, Ctrl-PgUp/PgDn, Ctrl-Tab, the buffer pickers, the split view commands, and the unsaved changes question before quitting or replacing the buffers. |
| `save` | Saving one document or every modified one, with the questions on the way (save as, overwrite, changed on disk). |
| `picker` | The filterable full window list used for buffers and shortcuts. |
| `complete` | Tab completion of paths in the Ctrl-O and save as prompts, and expanding a leading `~`. |
| `term` | The terminal session: raw mode, startup handshake, alternate screen, keyboard protocol, bracketed paste, cursor shape, window size, waiting for input or a resize, and restoring the terminal on every exit path. |
| `input` | Decodes the bytes read from the terminal into events: keys, pastes, cursor position reports and keyboard protocol reports. Pure parsing, no I/O. |
| `key` | The `Key` type (code plus modifiers) and key names such as `ctrl+shift+up`. |
| `keymap` | The binding table: key, command and help text. The single source of truth for dispatch and the Alt-H list. |
| `screen` | A grid of styled cells drawn in full each frame. Flushing writes only the cells that changed, inside a synchronized update. |
| `theme` | Highlight groups (syntax, markup and interface), the style each one draws with, and which group a tree-sitter capture name uses. |
| `layout` | How one line sits on screen: grapheme clusters with their display columns and widths (tabs expanded), and the visual rows it wraps into. |
| `cursor` | The cursor set of a view: anchor, head and desired column per cursor, one of them primary. Following changes, and merging cursors that overlap. |
| `view` | A window onto a buffer: screen area, gutter, cursors, scroll position, stepping over visual rows and keeping the cursor in sight. |
| `move` | Cursor movement and selection commands, written once against visual rows. |
| `bracket` | Finding the bracket that matches another, and the closest pair around a range. |
| `multicursor` | Adding cursors: at the next occurrence of the selection (Ctrl-E) and on the row above or below (Ctrl-Alt-Up/Down). |
| `edit` | Editing commands: typing, line breaks, indentation, comments, deletion, line commands (duplicate, delete, move, join, sort, trim), copy, cut and paste. Each edits at every cursor as one undo step. |
| `prompt` | Questions asked in the message bar: a line of text, or a choice of letters. |
| `clipboard` | Copy and paste through `wl-copy` and `wl-paste`, with an in-process fallback. |
| `render` | Draws a frame: one view or two side by side (gutter, syntax coloured text, selections, matches, whitespace symbols, ruler), a status bar under each, and the message bar. |
| `pattern` | The literal search pattern shared by find, replace and the match commands: escapes, anchors, case folding, whole words, and finding a match in a line. |
| `spell` | The word list compiled in from `data/words.txt`, and finding unknown words in a line. |
| `match` | The match set a document highlights (a pattern's matches or unknown words, limited to the text spell check looks at): finding the matches on a line, the next or previous one in the buffer, and counting them. |
| `search` | The search commands: Ctrl-F, Alt-N and Alt-P, Ctrl-W, Ctrl-R and Alt-S, and the pattern prompt of project search. |
| `lang` | The languages by file extension: display name and comment markers. |
| `query` | A compiled tree-sitter query with what the runtime leaves to its user: the text predicates (`#eq?`, `#match?`, `#any-of?` and their `#not-` forms) and the injection settings of `#set!`. Knows nothing about particular languages. |
| `highlight` | The grammar table and the only place that knows grammars: parsing a buffer (again incrementally after edits), injected languages, and the syntax group of every byte of the lines around the one asked for. Also tells spell check which text it looks at. |
| `project` | Project search (Alt-Shift-F, Alt-F): the scope, searching loaded files a slice at a time, the hit list and its preview. |

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

Measuring a column, finding the byte at a column, and starting to draw part way along a line all start from the line's beginning, since columns depend on everything before them. `LayoutBeginAt` and `LayoutBeginAtColumn` pass over runs of printable ASCII a byte at a time without reading clusters (a byte followed by a non-ASCII byte is left to the full rules, since a combining mark could join it), so on a 17 MB line a column takes about 5 ms instead of 35.

`LayoutWrap` splits a line into visual rows. A row breaks after its last space or tab that fits, or else just before the cluster that does not fit. Wrapped rows have no continuation indent. When the last row is exactly full, an empty row follows it, so the cursor at the end of the line has a cell to sit in. Without soft wrap a line is one row.

### Visual rows

A `ViewRow` is a line plus the index of one of its rows. `ViewStep` moves forward or back by any number of visual rows and stops at the ends of the buffer. Vertical movement, paging, scrolling and the scroll margin are all written against `ViewStep`, so the same code serves wrapped and unwrapped views. Nothing holds a row index for the whole buffer. A wrapped line is laid out when it is reached, so the cost depends on the rows stepped over, not the buffer size. Without soft wrap every line is one row, and stepping is plain arithmetic on line numbers, so a jump across a million lines is instant. A wrapped line of 4 KB or more keeps its rows in a small cache in the view (four lines, emptied by any change to the buffer), because scrolling and drawing ask for the same line's rows many times a frame.

Each cursor keeps a desired column: its column within its visual row. Up, Down, PageUp and PageDown aim for it, and every other movement resets it. Home and End act on the logical line. Up on the first row goes to the start of the buffer, and Down on the last row to the end.

### Scrolling

The view keeps its first visible row (`top`) and, without soft wrap, its first visible column (`leftColumn`). `ViewScrollToCursor` runs before every frame, so movement, resizing and toggles all end with the cursors in sight. It keeps `scrollOffset` rows between the cursor and the top and bottom edges (capped at just under half the height). With several cursors whose heads span fewer rows than the view, it keeps the whole span in sight instead, with the margins when they fit and without them when they do not, so adding cursors does not scroll the first ones away. A span taller than the view falls back to the primary cursor. The margin shrinks to the rows that exist before or after the cursor, which is how it yields at the start and end of the buffer. The view never scrolls so far that empty rows show below the end of the buffer. Columns get the same margin, and it yields at the start of the line.

Ctrl-Up and Ctrl-Down move `top` by one row and leave the cursor alone. They refuse (with a message) when a cursor being kept in sight would end up inside the margin, or the view would pass either end. PageUp and PageDown move both the cursor and `top` by the height less one row, so the cursor keeps its screen row.

### Rendering

`RenderFrame` gives the view every row except the bottom two (the status bar and the message bar), scrolls it to its cursor and draws it. With a split it draws two panes, left to right, either side of a one column separator, each with its own status bar. The gutter is the line number right aligned with a space either side, at least three digits wide. Only the first row of a wrapped line is numbered. With whitespace display on, a space is drawn as `·` and a tab as `→` in its first cell, in the `whitespace` colour. The ruler is drawn as `│` in the cell just after `rulerColumn` before the text, so text covers it, and spaces and tabs let it show through. A cluster cut by the left or right edge of the text area shows as blank cells. Drawing a line starts at its first visible row, or without soft wrap at the cluster under the first visible column, so the part of a long line before the view is never laid out cluster by cluster.

With more than one cursor, the terminal cursor is hidden and every cursor (the primary one too) is drawn by switching its cell to the `cursor` style (reverse video on the default colours) after the text is drawn, so it reads as a block whatever the cell held. A cursor at the end of a line covers the empty cell after it. Cursors outside the view are skipped by their line number before anything is measured.

A selection is drawn with the `selection` background behind the text's own colours. When a selection continues onto the next line, one selected cell after the end of the line shows that the line break is included.

The status bar shows the name (shortened in the middle to fit) with `[+]` and `[RO]` on the left, and the cursor count (with more than one), the wrap state, file type, line ending, `line:column`, line count and `open:loaded` buffer counts on the right. When the window is too narrow it keeps only `line:column`, and then nothing, on the right. Below `minWindowWidth` by `minWindowHeight` the editor shows only "terminal too small".

## Editing

### Cursors follow changes

A view listens to its buffer. After every `BufferReplace` it moves each cursor with `BufferAdjust`: a position before the change stays, one inside the replaced text moves to its start, and one at or after its end moves with the text that follows. The view's first visible line shifts the same way. So a command edits at one cursor and the others stay on their text without the command knowing about them. The other view of a split follows the same way.

An edit command loops over the cursors, replaces each one's range, and puts that cursor after its new text. When the loop ends, cursors that now overlap are merged and every desired column is reset. Commands that rewrite lines replace only the bytes that change, so every view of the buffer, not just the one editing, keeps its cursors on the same text: indent and dedent make one replace per changed line, Ctrl-D inserts the copy above the lines, moving lines takes the neighbouring line out and puts it back on the other side (so the moved lines are never replaced; cursors on the neighbour in another view move to the end of the moved lines), each join replaces only a line break and the blanks after it, and trimming replaces only each line's trailing blanks. Sorting is the exception: it replaces the whole block of lines at once, so another view's cursors inside it move to its start. A sort orders line indexes with a stable merge sort (insertion sort for short runs) and builds the new text once; the case folded orders skip the common prefix of two lines up to its last ASCII byte (which always ends a code point) before folding, which roughly halves their time. 1M lines sort in about 0.5 s by bytes and 0.75 s ignoring case. Indent and dedent then put the editing view's cursors back themselves, so a cursor at the start of a line stays before the new indentation. Ctrl-D, Ctrl-K, Ctrl-Shift-Up/Down, Alt-J and Alt-Q first collapse to the primary cursor, and so do the commands that place one cursor (Ctrl-L, Ctrl-M, Alt-M and the search commands).

### Multiple cursors

The primary cursor is the one most recently added. Esc collapses to it, the status bar reports its position, and the view follows it when the cursors do not all fit. Movement merges cursors that meet, as edits do. Merging finds it again by value after sorting, so it survives the cursors around it being joined.

Ctrl-E with no selection at the primary cursor selects the word at every cursor that sits on or just after one. With a selection on one line, it escapes the selected text into a case matching pattern (whole word when the selection is exactly one word) and searches forward from the selection's end with `MatchFind`. Matches that overlap any cursor are passed over, and the search stops once it has gone round the end and come back to the primary selection, so pressing it again after the last occurrence reports "no other occurrence" instead of looping. Ctrl-Alt-Up and Ctrl-Alt-Down step one visual row from the first or last cursor (from the outer end of its selection, so the new cursor never lands inside it) and keep its desired column, so a column of cursors goes down a ragged block of lines at one column.

A paste with as many lines as there are cursors (not counting the empty line after the last line break of a line wise copy) gives the cursors one line each, in buffer order. Otherwise each cursor gets the whole text.

The cost of an edit grows with the square of the cursor count, because each cursor's replace adjusts every other cursor. With 10,000 cursors, typing a character takes about 0.2 seconds.

Enter copies the indentation before the cursor and adds one level after `{`, `[` or `(`. With the matching closer right after the cursor it puts the closer on its own line below. Blanks after the cursor are dropped, and a line that holds only blanks before the cursor is left empty rather than with trailing blanks. One level is `tabWidth` spaces, or a tab when expand tab is off (Alt-T).

### Undo

`undo` listens to the buffer too, and records each change as `{start, oldText, newText}`. Everything between `UndoBegin` and `UndoEnd` (which nest) is one group, which also keeps copies of the cursor set from before and after. Undo replays a group's records backwards as inverse replaces and restores the "before" cursors; redo replays them forwards and restores the "after" cursors. A change outside a group is a programming error and ends the editor through `Fatal`, so nothing can slip past the history.

A typing, backspace or delete edit joins the last group when it is of the same kind, nothing has been undone, and nothing broke the run. `app` calls `UndoBreak` before every command except backspace and delete, and before a paste. Within a group, a change that simply continues the last record (typing after it, backspacing before it, deleting at the same place) extends that record, so typing a line costs one record.

The history keeps a save point: the number of applied groups when the text matched the file. The buffer is modified when the history is not at its save point, or its line ending differs from the one on disk, so undoing back to the saved text, or switching the line ending back, clears `[+]`. Starting a new group after an undo drops the groups that could have been redone, and the save point with them when it was among them. A reload clears the history. There is no depth limit.

### Prompts

The message bar doubles as the prompt. `PromptAsk` edits one line of text (Left, Right, Home, End, Backspace, Ctrl-Backspace, Delete, paste up to the first line break), and `PromptChoose` waits for one of a few letters (a capital letter counts as its lowercase choice, unless it is a choice of its own, as `U` is for sort). Escape cancels either. The answer goes to a callback, and the prompt is closed before the callback runs, so a callback can open the next prompt. `PromptHandle` adds two optional handlers to a text prompt: one told about every change to the text (the picker filters with it), and one asked to complete the text on Tab (Ctrl-O uses it).

### Save flow

`save` runs a queue of documents: the current one for Ctrl-S, every modified one for Ctrl-Shift-S and for "save all" when quitting or replacing the buffers. Each document is trimmed (when `trimTrailingWhitespace` is set), then written, and the next one starts from the completion of the last. A document without a name asks for one first, and during save all it is made current so the user can see what they are naming. A name that exists asks "overwrite?", and `FileStatusChanged` asks "changed on disk; overwrite?". Any error, refusal or cancel stops the queue, and the callback waiting for the end of the run (quit, replace) never runs. A save that fails after save as leaves the buffer unnamed again.

### Clipboard

Copy sends the text to `wl-copy` on standard input, and paste reads `wl-paste --no-newline --type text`. Both run with `posix_spawnp`, standard error to `/dev/null`, and a two second limit, after which the command is killed. The editor ignores SIGPIPE so a command that exits early cannot end it, and resets SIGPIPE to the default for the command. Without `WAYLAND_DISPLAY`, or when the command fails, the in-process copy is used.

Copy with no selection is line wise: each cursor's line, ending in `\n`. The clipboard remembers the last copy and whether it was line wise. When a paste finds exactly that text, it is inserted as whole lines above the cursor's line instead of at the cursor.

A paste, from Ctrl-V or a bracketed terminal paste, has `\r\n` turned into `\n` (a lone `\r` stays, as when loading) and is one undo step without any auto indentation.

### Saving and reloading

`document` keeps the file's line ending from its last load or save. Saving runs trailing whitespace trimming first when `trimTrailingWhitespace` is set (one undo step, so the screen matches the file), then `FileSave`, then marks the save point. Reload reads the file into a separate buffer and swaps the text in only when that worked, then clamps the view's cursors and scroll position into the new text.

### Split view

Each document has two views, one per slot, and each side of the split uses its own slot of every document. The focused side's entry is the workspace's current entry, so every command works on the focused view without knowing about the split. The workspace also keeps the entry the other side shows and which slot is on the left, so Ctrl-T only flips that, and Ctrl-J swaps the current and other entries along with the focused slot. Ctrl-Alt-Right and Ctrl-B put the new view in the other slot, on the right.

A document's `view` points to the view of the slot that last had focus on it, which is the one commands use. When a side shows a document whose `view` is the other slot's, it first copies that view's cursors and scroll position into its own, so a buffer opens where it was last seen rather than where this side last left it. A side that switches back to a buffer it was the last to use keeps its own position. Both views stay buffer listeners for the document's lifetime, so an edit through either one moves the other's cursors and scroll position as well. Undo restores the cursors only in the view that undoes.

Below `minSplitWidth` (80) columns only the focused view is drawn, at full width, and nothing about the split changes, so widening the window brings both back. Only the focused view shows its cursors, and pickers and prompts span the whole window. Replacing the buffers with a folder resets the workspace, which ends the split.

## Search

### Patterns

One pattern syntax serves Ctrl-F, Ctrl-R, Alt-N, Alt-P and project search. A pattern is literal text. A `^` at its start and a `$` at its end are anchors, and the escapes `\t`, `\r`, `\n`, `\\`, `\^` and `\$` stand for those characters; any other backslash is literal. A match never crosses a line break, so a pattern holding `\n` never matches, and an empty pattern matches nothing, so matches are never empty and stepping through them always moves.

Without case matching (the default, toggled with Ctrl-T for the session), the pattern is kept as case folded code points, and the text is folded one code point at a time as it is compared, with a fast path for ASCII. Folding can change a character's length (the Kelvin sign folds to `k`), so offsets are always those of the original text. A match must start and end on grapheme cluster boundaries, so `e` does not match the start of an `e` with a combining accent. Ctrl-W builds a whole word, case matching pattern from the word at the cursor (word characters as for word movement).

### Match sets

Matches are never stored. Each document has a `marks` kind (none, search or spell), and the last search pattern is shared by every document. Rendering finds the matches on each visible line as it draws it, and Alt-N and Alt-P scan from the cursor, line by line, going round the end of the buffer once. So an edit can never leave a highlight or a step out of date, and the cost is proportional to the lines drawn or scanned. A match that is also selected is drawn in a stronger colour, which is how the current match stands out. Esc clears multiple cursors first, then the selection, then the highlights.

Incremental find saves the cursors, scroll position, marks kind and last pattern when it starts, and puts all of them back on Esc. Each change to the prompt searches again from where it started, so deleting a character goes back to an earlier match. The search prompts take Up, Down, Ctrl-N, Ctrl-P and Ctrl-T before the prompt itself sees them.

### Replace

Ctrl-R runs from the start of the selection to the end of the buffer, then from the top back to where it started. That stop position moves with each replacement through `BufferAdjust`, and every search continues after the last replacement, so replacing `a` with `aa` cannot loop. The run is one undo group, opened before the first replacement and closed when the run ends (answers come one prompt at a time, but the prompt takes every key, so nothing else can edit in between). Replacing all rebuilds each line once and replaces it with a single change, so 100,000 matches take about 0.1 seconds.

### Spell check

The word list is embedded with `#embed` and indexed (one offset per word) the first time it is used, then binary searched in byte order. A run of letters, digits, `_` and apostrophes is read whole: runs with digits or `_` are skipped, apostrophes at either end are trimmed, words in capitals only are taken for acronyms, and words inside a blank separated chunk holding `://`, `www.` or `@` are skipped as links. A word is known when it is in the list as written, or when it starts with a capital and its lowercase form is. Typographic apostrophes are looked up as `'`. Letters are those of the Latin, Greek and Cyrillic scripts; other scripts are not checked. In a highlighted buffer, the spell match set asks `HighlightSpellable` about each unknown word's first byte and passes over the ones outside the text it checks (see Highlighting). Other buffers are checked in full.

### Project search

Alt-Shift-F takes its pattern through the search prompt (so Ctrl-T and recall work as in Ctrl-F), and the pattern becomes the last search. The scope prompt then splits its answer at blanks into two `filter` lists, the patterns to pick and the `!` patterns to leave out, and tests each entry's display path (after any leading `./`) against both. An unnamed buffer has no path, so only a scope with nothing to pick takes it. Entries already refused as binary or too large are counted as skipped without being read.

The search never blocks the main loop. `ProjectContinue` searches entries for about 20 ms, then returns so the loop can draw the progress and read keys (it polls with no timeout while a search runs). While it runs, every key except Esc is ignored. Open entries are searched from their buffer, the others are read into one reused buffer, checked for size and a nul byte as on load, and scanned line by line with `\r` before a line break left out, so line numbers match what opening the file gives. A case-sensitive search first checks the whole file with `memmem`. A matching line is recorded once, with its first match and up to 200 bytes of its text, and the search stops at `maxProjectHits` lines. Searching 100,000 small files from the page cache takes about half a second. Nothing is opened, so the open count stays the same.

The hit list is a picker with a preview: `PickerShowPreview` gives the picker a function that draws beside the list when the window is at least 80 columns wide. Without a preview the picker shows each item's note, here the line's text. The preview reads an unopened file again (keeping the last one read) and draws the lines around the hit with their numbers, scrolled sideways when the match would be off screen, and every match highlighted. Opening a hit switches to its entry (opening it) and selects the match on the stored line, or the line's start when the text has changed. A folder opened with Ctrl-O clears the list, because entry indexes no longer mean the same files.

## Buffers

### Loading

`loader` turns paths into a `LoaderList` of display paths and file ids (device and inode). A path that does not exist is a new file, unless it ends in `/`, which is an error. A folder is walked depth first with `openat` and `fdopendir`, and each listing is sorted by name. Entry types come from `d_type`, so a regular file costs no `stat`: its id is the folder's device and the entry's inode. A link is followed with `fstatat` and kept only when it leads to a regular file, so links to folders are skipped (and folders are opened with `O_NOFOLLOW` to be sure). Other kinds (pipes, sockets, devices, broken links) and `.git` folders are skipped. A folder that cannot be read is counted and reported in the message bar.

Files are de-duplicated by id in a hash set, so a file named twice, inside a named folder, or reached through a link appears once, under the first path that reached it. New files are de-duplicated by path. The walk stops as soon as it would pass the limit. At startup that ends the program with a message before the terminal changes; at runtime the operation is refused. Nothing but folder listings and `.editor` files is read, so a 100,000 file tree loads in well under a second.

### .editor files

Each folder in a walk is probed for `.editor`. When there is one, its patterns replace the ones from above for that folder and everything below, and paths are matched relative to it. Patterns come one per line; `#` at the start of a line or after a blank starts a comment. `filter` knows four forms: a name (`foo.txt`, `*.c`), a folder name (`bar/`), and either one tied to a path when it starts with `/` or has a `/` inside (`/foo/foo.txt`, `/*.c`, `/bar/`). `*` matches within one name and is the only wildcard. A folder is tested before it is entered, so a matched folder is never read, and a file also matches a folder pattern that matches any folder above it (which project search scopes rely on, so `src/` picks everything under any `src` folder). The folder that holds an `.editor` file is judged by the patterns above it, not its own. Files named on the command line are always loaded, and `.editor` files are only read when loading (startup and Ctrl-O on a folder).

### The buffer list

`workspace` holds one entry per loaded file or unnamed buffer: the path (until opened), the file id, the document once opened, a refusal reason, a number for unnamed buffers, and a "last used" tick. The document is allocated when the entry is first used, and its path moves into the document. A file over the size limit or with a nul byte in its first 4 KB is refused, remembered as such and never read again, and the current buffer stays. Other errors (permissions) are tried again next time. The status bar's `open:loaded` counts open documents and entries.

Each document owns its views (one per side of a split), which keep their cursors and scroll position while another buffer is shown, and keep following edits because they stay buffer listeners. App never keeps a document or view pointer across commands, since opening a folder frees every document.

At startup with several paths or any folder, the first entry that opens is made current and the buffer picker opens over it, so Escape leaves you in it. With none openable, or none at all, an unnamed buffer is used. Ctrl-O on a file adds it (or finds it among the loaded entries) and switches to it. On a folder it first loads the new list, refusing an empty or oversized one, then asks about unsaved changes, then frees everything and loads the new list as at startup.

Ctrl-PgUp and Ctrl-PgDn step through open entries in load order, wrapping. Ctrl-Tab goes to the most recently used entry other than the current one. The buffer pickers list entries most recently used first, then named ones by path, then unnamed ones, with a note for unsaved changes or a refusal.

### Pickers

`picker` takes a list of items (text, an optional note, an id) and shows it over every row above the message bar, with a bar naming the list and counting matches. The filter is a text prompt with a change handler, so it edits like any prompt. Up, Down, PageUp, PageDown, Home and End are taken by the picker first, and Enter with no matches does nothing. Each item keeps a case folded copy of its text, so filtering 100,000 paths is a byte search per item. A filter matches anywhere in the item, `^` at its start ties it to the start and `$` at its end to the end, and both ignore case. Long items are shortened in the middle. The Alt-H list is built from the binding table, sorted by description, and Enter runs the picked command.

## Highlighting

### Languages

`lang` maps an extension (ignoring case) to a display name and comment markers. A document picks its language when it opens and again after each save, so saving under a new extension changes it. `highlight` looks its grammar up by that display name in `grammars[]`. Each row holds the injection id other grammars use to embed it, its `tree_sitter_<lang>()` entry point, its highlight query and optional injection query (`u8` arrays filled by `#embed`), and two flags: `prose` (spell checked outside code) and `freshParse` (see below). A grammar's queries are compiled the first time any buffer uses it and shared from then on. A query that fails to compile is reported in the message bar and leaves that language uncoloured. README.md lists the steps for adding a language.

### Parsing

A highlighter listens to its buffer. Each change becomes a `TSInputEdit` applied to the trees at once, and the parse itself waits until a line is asked for, so a burst of edits (many cursors, replace all) costs one reparse. Tree-sitter needs the byte offset of each change, so the highlighter keeps the offsets of line starts, extending them lazily and keeping only those up to the changed line. The parser reads the buffer through a `TSInput` callback, which hands over the rest of the line at the requested point and then its line break, so the text is never copied into one string.

Parsing has a deadline of `maxParseTime` (one second): a parse that runs past it is stopped, and highlighting stays off for that buffer until it is reloaded. Buffers over `maxHighlightSize` (16 MB) are not parsed at all. In both cases the status bar shows the language as "(uncoloured)". A 20,000 line C file parses in about 0.1 seconds, and each keystroke after that reparses in under 2 ms.

### Injections

A grammar with an injection query embeds others: Markdown its inline grammar (for every paragraph and heading), fenced code blocks and HTML blocks; HTML its scripts and styles; inline Markdown its HTML tags. The injected language comes from `#set! injection.language` or from the text of an `@injection.language` capture, and is looked up first by injection id, then as a file extension through `lang`, so a fence can say `js`. Languages without a grammar (yaml, regex, jsdoc) are skipped. An injected language covers the ranges of its `@injection.content` node, less the node's named children, which belong to the outer language (the `> ` of a quoted code block). Anonymous children are only tokens of the outer grammar, so they stay in. `injection.combined` is not supported: each node is parsed on its own.

Each injection is a layer: a grammar, its ranges and its tree. Layers are found again by grammar and first byte, and follow edits like the main tree, so an unchanged paragraph or code block is not parsed again. An edited layer is parsed incrementally, except for grammars marked `freshParse`: Markdown's inline grammar gives different trees when parsed incrementally (found by comparing random edits against fresh parses), and its layers are a paragraph each, so they are parsed from scratch. Layers nest up to four deep. Once there are more than 512, those not used by the last two windows are dropped.

### Colouring lines

`HighlightLine` gives a byte per text byte: the syntax group in the low six bits, plus two flags for spell check. It fills a window of lines (64 before the line asked for and 192 after) by running each layer's highlight query over just those rows. Captures that fail their predicates are dropped. The rest are sorted by start, enclosing nodes before the nodes inside them, and for the same node by pattern, then painted in that order, so an inner node beats the node around it and a later pattern beats an earlier one (the convention of the vendored queries, as in `(identifier) @variable` followed by more specific patterns). Injected layers are painted after the layer that holds them. Two windows are kept, one for each side of a split, and any edit invalidates both.

A line over 16 KB (minified code) is coloured only around the part being drawn: from 16 KB before the first byte drawn to 64 KB after it. The window runs the queries once for each run of ordinary lines and once for each long line's span, and clips every capture to the part it was run for, so a node reaching into another part cannot paint over it. Other long lines in the window are coloured around the same byte offset, which suits an unwrapped view scrolled sideways. Asking for a long line outside its span fills the window again. So a keystroke on a 1 MB minified line costs about 30 ms rather than 300. Rendering asks for the groups of each line it draws and starts from that group's style, with selections and matches drawn over it.

A capture name picks the group that shares its first component (`string.special` is `string`), except that any `*.builtin` is `builtin`, and Markdown's `text.*` captures use their second component: `title`, `literal`, `emphasis`, `strong`, `uri` and `reference`, which are markup groups in the theme. A capture with no group colours nothing, and `@none` (which Markdown puts on code block contents) clears the colour.

Tree-sitter leaves text predicates to its user. `query` evaluates `#eq?`, `#match?` and `#any-of?` (and `#not-` forms) against the captured text. `#match?` uses POSIX extended regular expressions, with `\d`, `\w` and `\s` rewritten as bracket expressions first, since the queries are written for Rust's regular expressions. Other predicates (`#is-not? local`, `#offset!`) are ignored.

### Spell check scope

Painting `literal` or `uri` also sets a sticky literal flag that later paints do not clear, so the contents of a code block stay marked as code even though `@none` and the injected language recolour them. A `string.special` capture (a CSS colour, a regular expression, a JSON key) sets a special flag. Spell check passes over literal and special bytes, and otherwise looks at comments and strings in every language, and in prose languages also at uncoloured text and markup (headings, emphasis, link text).

## Performance

Measured on the release build, timed from the key to the end of the frame:

- A 100 MB file of 1.3 million lines loads in about half a second. Moving, typing, paging and undo each take under 10 ms. Indenting every line takes 0.3 seconds, and spell check (which counts every unknown word) about 3 seconds.
- A 17 MB line without soft wrap takes about 35 ms a key, and with soft wrap 5 to 75 ms (more right after an edit, when its rows are laid out again).
- Tree-sitter highlighting is skipped above 16 MB, and stops for a buffer whose parse takes over a second (a 7 MB minified file does after its first edit). The first edit after parsing a 3 MB Markdown file pauses for about 0.3 seconds, and typing in a 2,000 line fenced block costs about 35 ms, because the Markdown grammar scans the whole block again.
- 100,000 files load and draw the picker in about 60 ms from the page cache, filtering them takes 2 to 4 ms a key, and searching them all takes about 0.35 seconds.
- An edit with 1,000 cursors takes 4 to 22 ms, and with 10,000 about 0.2 seconds (see Multiple cursors).

## Comments

Ctrl-/ uses the comment markers from `lang`. It first decides for all the lines touched by every cursor: when each of them that is not blank is commented already, it uncomments, otherwise it comments. Commenting puts the start marker and a space at the smallest indentation of each block of touched lines (so the markers line up) and, for block comment languages, a space and the end marker at the end of the line. Blank lines are left alone unless every touched line is blank. Uncommenting removes the markers and one space next to each. It runs as line edits like indentation (one replace per line, in a second pass for end markers), so other views keep their cursors on the same text, and the whole toggle is one undo step.

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

## Unicode tables

`src/unicodedata.c` is generated and committed. To regenerate it, for example for a new Unicode version:

1. Download `UnicodeData.txt`, `EastAsianWidth.txt`, `CaseFolding.txt`, `auxiliary/GraphemeBreakProperty.txt` and `emoji/emoji-data.txt` from `https://www.unicode.org/Public/<version>/ucd/` into one folder.
2. Run `tools/unicodedata.py <folder> > src/unicodedata.c`.

The current tables are Unicode 16.0.0. Width rules: East Asian Wide and Fullwidth are 2 columns. Nonspacing marks, enclosing marks, format characters (except the soft hyphen), Hangul medial vowels and final consonants, and U+200B are 0. Everything else is 1.
