# Architecture

This document describes how the editor is put together: the modules and how they layer, how a key press flows through them, the rules that hold across modules, and the decisions behind the design.
What one module does in detail is in the comment at the top of its header, and how it does it is in its source; this document only repeats them where a rule spans modules.
README.md describes the editor as a user sees it.

## Layout

- `src/`: every editor source, flat, `.c` and `.h` together.
- `vendor/tree-sitter/`: the tree-sitter runtime (`include/`, `src/`), compiled through its `src/lib.c` amalgamation.
- `vendor/grammars/<id>/`: generated grammar sources (`parser.c`, an optional `scanner.c`) and their `tree_sitter/` headers.
  PHP and XML keep upstream's layout (`php/src/`, `common/scanner.h`), because their scanners include a shared header by a relative path.
- `vendor/patches/`: the editor's patches to vendored code, one diff each (see The default packages).
- `vendor/queries/<id>/`: `highlights.scm` and, where the grammar embeds other languages, `injections.scm`.
- `languages/<id>/`: the editor's own files for each default package, its `language` file and, for a language with symbols, `tags.scm`.
  They are not vendored, so they are edited freely.
- `data/words.txt`: the default spell check word list, one word per line, in byte order (`LC_ALL=C`).
  `build.sh --install` copies it to the config folder when there is no list there.
- `tools/`: scripts outside the binary.
  `unicodedata.py` generates `src/unicodedata.c`, `bindings.py` writes README.md's key binding tables from `src/keymap.c`, and `language.sh` makes a language package from a grammar repository.
- `build.sh`: the build.
  It writes `./editor` and `compile_commands.json` (each source's flags, for clangd), with objects in `build/`.
  `--languages` builds the default packages into `build/languages/<id>/`, and `--install` copies them and the word list to the config folder.

## Glossary

- **Entry**: one item of the buffer list (`WorkspaceEntry`), a loaded file or an unnamed buffer.
- **Loaded**: a file has an entry.
- **Open**: an entry whose file has been read into a document.
  Files from folders are loaded at startup, but only opened when first used.
- **Document**: an open entry's buffer with its file, undo history, views, highlighter, changes and language server document.
  An image is a document too, with an empty buffer.
- **View and slot**: a window onto a document's buffer, with its own cursors and scroll position.
  Every document has two views, one per slot, and each side of a split uses one slot.
- **Serial**: a number given to each entry when it is added, never reused, which other modules keep instead of an index or a pointer.
- **Revision**: a buffer's count of changes.
  Anything worked out from the text keeps the revision it was worked out at, to tell when it is stale.
- **Stamp**: a file's device, inode, size and modification and change times (`FileStamp`), to tell when it changed on disk.
- **Scan**: a walk over many entries a slice at a time between frames (project search, the symbol index, Ctrl-G).
- **Layer**: one parsed language in a buffer: the buffer's own grammar, or one injected into it, such as a code block in Markdown.
- **Hunk**: one run of lines that differ between a buffer and its file in the last git commit.
- **Display column**: a 1-based screen column within a line, with tabs expanded and wide characters counted as two.
  The status bar, go to line and the ruler use it.

## Data flow

The editor is one thread around one loop in `app`:

1. **Input.** `TermWait` polls standard input, the resize pipe and the language servers' pipes, with a timeout from the earliest deadline (a server request, the hover, the change marks, the recovery copy).
   `input` decodes the bytes into events: keys, pastes, mouse reports, focus and terminal replies.
2. **Dispatch.** A running scan, learning mode, an open picker, the search prompts and any other prompt take a key first, in that order.
   Otherwise a printable key is typed, and any other key is looked up in `keymap`, and `app` runs its command.
3. **Commands** live in the modules that own what they change (`move`, `edit`, `search`, `session`, `save`, `project` and so on).
   They work on the current document's focused view through `workspace`, and change text only through `BufferReplace`.
4. **Listeners.** Every replace tells the buffer's listeners: both views of the document (cursors and scroll position), its undo history (records), its highlighter (tree edits) and its language server document (pending changes and diagnostic ranges).
   So a command does not know who shows or follows the text.
5. **Servers.** `LspPump` sends each language server the changes made since the last pass, one message per document, and handles what the servers sent.
6. **Frame.** Once no input is waiting, `RenderFrame` scrolls each view to its cursors and draws the views, status bars and message bar into the `screen` grid, laying out only the visible rows and colouring only the visible lines.
   `ScreenFlush` writes the cells that changed.

Loading runs before the loop: `loader` turns the command line into a list of files, `workspace` holds them as entries, and an entry's file is read into a document the first time it is used.

## Modules

The groups run from the lowest layer up, and a module includes only modules of its own group or the groups before it, so the includes have no cycles.
The one exception is `app.h`, which declares the shared `workspace` and `message` that the command modules (and `learn`, `lsp`, `preview` and `scan`) use.

**Foundations**
- `types.h`: the fixed width numeric aliases (`u8`..`u64`, `i8`..`i64`, `f32`, `f64`), the only numeric types used.
- `settings.h`: the built-in settings as `constexpr` values; a `Default` suffix marks the start of a runtime toggle.
- `fatal`: restores the terminal through a registered hook, prints a message and exits.
- `memory`: checked allocation, so callers never test for null.
- `strbuf`: a growable byte string; a zeroed one is empty and valid.
- `unicodedata`: generated Unicode tables (see Unicode tables).
- `utf8`: decoding, encoding, grapheme cluster boundaries, display width, case folding and mapping.

**Text and files**
- `buffer`: a buffer's lines, and `BufferReplace`, the one edit.
- `file`: reading, loading and saving files, their stamps, and the config and state folders.
- `undo`: a buffer's undo history, in groups with the cursors before and after.
- `cursor`: a view's cursor set, following changes and merging.
- `layout`: the only place that turns bytes into display columns and wrapped rows.
- `view`: a window onto a buffer: area, gutter, cursors, scrolling, and the position at a cell.
- `filter`: the path patterns of `.editor` files and project search scopes.
- `loader`: turns paths into the list of files to load.
- `complete`: Tab completion of paths in prompts.

**Terminal**
- `term`: the terminal session: raw mode, handshake, modes, window title and folder, size, waiting for input, and restoring on every exit.
- `input`: decodes what foot sends into events, with no I/O.
- `key`: the `Key` type and key names.
- `keymap`: the binding table, the one source for dispatch, Ctrl-H, learning mode and README's tables.
- `learn`: learning mode (Alt-H).
- `screen`: the cell grid, flushed as differences inside a synchronized update, and images placed over it.
- `theme`: the highlight groups, their styles, and the group of each capture name.
- `prompt`: questions in the message bar.
- `clipboard`: `wl-copy` and `wl-paste`, with an in-process copy.

**Languages and search**
- `lang`: the registry of language packages.
- `query`: a compiled tree-sitter query with the text predicates the runtime leaves to its user.
- `highlight`: the only module that knows grammars: parsing, layers, colouring, symbols and syntax nodes.
- `pattern`: the search pattern shared by every search, literal or regular.
- `regex`: the regular expression engine.
- `spell`: the word list and unknown words.
- `match`: the match set a document highlights.
- `bracket`: matching brackets.

**Images**
- `inflate`: zlib decoding, for git objects and PNG.
- `bitmap`: 8-bit RGBA pixels, shrinking, cutting, flattening and turning.
- `png`, `gif`, `bmp`, `jpeg`: the decoders.
- `sixel`: the sequence that draws a bitmap.
- `image`: an image file opened for viewing.

**Git and language servers**
- `git`: reads a file as the last commit has it.
- `diff`: the hunks between two texts.
- `changes`: a document's hunks against the last commit.
- `json`: reading and writing JSON messages.
- `rpc`: a child process speaking JSON-RPC, never blocking.
- `lsp`: the language server client, owning no commands.
- `diagnostics`: a document's diagnostics, following edits.

**Documents and the workspace**
- `document`: a buffer with everything that follows it, or an image.
- `workspace`: the buffer list, serials, the current entry and the split.
- `render`: draws a frame, and remembers where it drew each pane for the mouse.
- `picker`: the filterable full-window list.
- `preview`: the lines around a place, drawn beside a picker.
- `scan`: walking entries a slice at a time.
- `recover`: copies of unsaved buffers in the state folder.

**Commands**
- `move`: movement and selection, including syntax selection and wheel scrolling.
- `edit`: typing, deleting, line commands, case, alignment, clipboard.
- `multicursor`: adding and removing cursors.
- `mouse`: what the mouse does to a view.
- `markdown`: aligning Markdown tables.
- `search`: find, replace, the match commands and the search prompts.
- `project`: project search and replace.
- `jump`: jump history.
- `places`: a picker of `file:line` places.
- `symbols`: the symbol pickers and the project symbol index.
- `multifile`: changes made to several files at once, and undoing them together.
- `review`: the git change commands.
- `issues`: the diagnostic commands.
- `lookup`: the language server questions (definition, uses, hover, other file).
- `refactor`: rename, fixes and formatting.
- `save`: the save queue and the file menu.
- `session`: loading, switching and closing buffers and folders, and the split.
- `app`: startup, the main loop, dispatch and the commands that fit nowhere else.
- `main`: calls `AppRun`.

## Invariants

These rules span modules, so no single header can state them.

**One edit primitive.** Every change to a buffer is `BufferReplace(range, text)`: insertion is an empty range, deletion empty text, and undo and redo are inverse replaces.
`BufferSetText` (loading) and `BufferSwapText` (a reload) replace everything without telling listeners, and their callers reset what followed the old text.
Every replace, set and swap adds one to the buffer's revision.

**Listeners never edit.** A listener must not change the buffer while it is being told of a change.
Commands that rewrite lines replace only the bytes that change (indent replaces per line, a join only the line break and blanks, Ctrl-D inserts above), so every view, not just the editing one, keeps its cursors on the same text.
Sorting and table alignment rebuild a block in one replace, so another view's cursors inside it move to its start.

**Undo groups and the save point.** A change outside an undo group is a programming error and ends the editor through `Fatal`, so nothing slips past the history.
Groups nest; typing, backspace and delete join the last group of their kind until something breaks the run, and `app` breaks it before every other command and every paste.
The history keeps a save point, the number of applied groups when the text matched the file, so undoing back to the saved text clears `[+]`.
Starting a group after an undo drops the groups that could have been redone, and the save point with them when it was among them.

**Revisions and stamps mark staleness.** Nothing outside a buffer follows its edits except its listeners.
Everything else keeps the revision (or for a file not open, the stamp) it was worked out at and checks it before use: the change hunks, project hits, the symbol index, Alt-Left's stack and language server answers.
Matches are never stored at all (see Decisions).

**Serials outlive commands, indexes and pointers do not.** Anything kept across frames identifies an entry by its serial (or a path), never by its index, a document pointer or an inode, which every save changes.
Entries are only appended and `WorkspaceRemove` compacts without reordering, so serials stay sorted and `WorkspaceEntryOf` finds one by binary search.
A removed entry simply stops matching, so project hits, jump places, the file menu, the save queue and a close waiting on its save need no fixing up.
`app` never keeps a document or view pointer across commands, and the mouse keeps only rectangles, because opening a folder frees every document.

**One thread, nothing blocks.** There are no threads.
Long work is cut into slices (`scan`, about 20 ms each) so the loop draws progress and reads Esc between them, and while a scan runs it takes every key, so no entry is added or removed under it.
Language servers are pipes polled with the terminal, written and read only as far as they take; a request has a deadline, and a server that stops reading cannot block the editor.
Child processes (the clipboard) have time limits.

**One clock.** Deadlines and delays use `TermNow` (monotonic milliseconds), apart from `rpc`'s own `RpcNow`.
The parse deadline is processor time instead (see Highlighting).

**The trust boundary.** Language packages, whose `parser.so` and `server` run with the user's privileges, are only read from the config folder, never from a project.
Text from files, servers and file names reaches the terminal only through the screen grid, which shows control characters as `^X` or U+FFFD, and the window title replaces them with `?`.
Sizes read from git objects, PNG headers and server messages are checked against limits before memory is reserved.

**Exits restore the terminal.** `TermStop` only uses `write` and `tcsetattr`, so it is safe in a signal handler, and it runs on a normal exit, through `Fatal`, and from the crash signal handlers.
A hang-up (SIGHUP, SIGTERM, the end of input, a failed read or write) sets a flag instead, so the main loop can copy unsaved work before exiting normally (see Recovery).
Crash handlers write nothing but the terminal reset, since the heap may be what is damaged.

## Decisions

**Foot only.** The editor relies on foot's kitty keyboard protocol to tell Ctrl-I from Tab and Ctrl-Shift-S from Ctrl-S, and its decoder only understands the sequences foot sends with the disambiguate flag.
One terminal means one set of sequences to get right: sixel graphics, synchronized updates, OSC 2 and 7, focus reports and SGR mouse reports are used without fallbacks.
The handshake refuses any terminal without the protocol before the screen changes.

**A flat array of lines.** A buffer is a `StrBuf` per line.
An edit inside one line touches only that line, and every module can index a line directly.
The measured costs on a 100 MB file of 1.79 million lines (Performance) stay within a key's budget, so a rope or piece table would add complexity for no visible gain.
A loaded line is allocated at its exact length and grows by half, not double, so an edit to every line does not double the memory.

**Matches are never stored.** A document only keeps which kind of matches it shows (none, search or spell), and the last pattern is shared.
Rendering finds the matches on each line as it draws it, and Alt-N and Alt-P scan from the cursor.
So an edit can never leave a highlight or a step out of date, and the cost follows the lines drawn or scanned, not the buffer size.
The match count (`3/17`) is counted again each time, only in buffers up to 4 MB and up to 10,000 matches.

**Its own regular expressions.** libc's `regcomp` works on bytes in the C locale the editor runs in, so `.` would match one byte, case would fold only in ASCII and `\w` would disagree with the editor's words.
It also cannot search a slice of a line that is not nul terminated.
`regex` is a Pike VM, so search time grows with the line length times the pattern size and never exponentially, and the same engine serves tree-sitter's `#match?` predicates.

**Its own git reader.** The change marks only need one file's text as HEAD has it.
Reading the object store directly needs no `git` program or library, never blocks on a subprocess, and is a few milliseconds a file.
Objects are only looked up by names from refs, commits and trees, so nothing is hashed, and SHA-256 differs from SHA-1 only in a name's length.
Anything unusual (worktrees, submodules, reftable, bare repositories, filters) is refused rather than half supported.

**Its own image decoders.** The binary links nothing but libc (the tree-sitter runtime is vendored), so PNG, GIF, BMP and JPEG are decoded by the editor's own code, each tested against ImageMagick or libjpeg.
An image is a `Document` with an empty buffer rather than a new kind of entry, because everything from the main loop to the split relies on the current entry having a document and views; the empty buffer makes search, symbols and jumps find nothing in it.
Two guards keep it unchanged: `app` lets only the commands in `ViewOnly` run on an image, and `DocumentSave` refuses one.

**Grammars as packages.** Each language is a folder in the config folder with a `parser.so` loaded with `dlopen` on first use.
With the grammars out of the binary it went from 9.7 MB to 0.8 MB, a language is added without rebuilding the editor, and a broken grammar only leaves its language uncoloured.
The grammars use only libc, not the runtime, so the editor need not export its symbols.
Handles are never closed, as trees and queries point into the grammar.

**No file watching.** A file changed by another program is noticed when the window regains focus, when its buffer is switched to, and before a save.
An unmodified buffer is then reloaded, and a modified one is only noted.
These checks catch the usual case, a change made from another window, without watching every loaded file.

**Language servers never block, and never edit unasked.** Every message to a server is queued and written as the pipe takes it, every answer has a deadline, and an answer that finds the cursor, revision or entry changed is dropped.
`workspace/applyEdit` is refused, so a server can only change text as the answer to a command the user gave, and that change is one undo step per file that can be listed and undone as a whole.

**Recovery copies, never the original.** Unsaved work is copied to the state folder rather than saved over the user's file, which stays as the user last saved it.
Copies are written from the main loop after 2 s of rest and on a hang-up, never from a crash handler.
The names describe themselves, so there is no restore command.

**Compile-time settings.** Settings are `constexpr` values in `settings.h`, and the toggles a user wants during a session have keys.
There is no configuration file to parse, validate or document.

## Terminal and input

Startup (`TermStart`) checks that standard input and output are a terminal, saves the mode, installs the signal handlers, enters raw mode and sends the handshake: the keyboard protocol query (`CSI ? u`) followed by a cursor position request (`CSI 6 n`).
Every terminal answers the second, so a cursor report with no protocol report before it means this is not foot, and the editor exits; so does no answer within a second.
Only then does it push the window title, enter the alternate screen, push the keyboard flags (disambiguate only), and turn on bracketed paste, the cursor shape, mouse reporting (`?1002` presses, releases and drags, in the SGR form `?1006`) and focus reports (`?1004`).
`TermStop` undoes these in reverse: mouse reporting first, since it is not kept per screen, and the keyboard flags before leaving the alternate screen, since foot keeps a flag stack per screen.

The window title (OSC 2) and the folder for foot's new windows (OSC 7) are written once a frame, only when they change.
The title has control characters replaced with `?` and the folder every byte but unreserved ones and `/` percent-encoded, so a file name cannot end either sequence.
`TermStop` pops the title stack and gives back the folder the editor started in.

SIGWINCH writes a byte to a self-pipe, so a resize wakes the poll like input does.
`TIOCGWINSZ` gives the size in pixels as well as cells, so images need no query of their own.

`input` understands what foot sends with only the disambiguate flag.
Text arrives as UTF-8, unmodified Enter, Tab and Backspace as control bytes, and every other key with Ctrl or Alt, Escape and Shift-Tab as `CSI code ; modifiers u` with the unshifted code.
Cursor and editing keys use their legacy forms with modifiers, and function keys are skipped.
Any other escape sequence is noise and dropped whole: a control string up to BEL or ST, SS3 with its final byte, or else the escape with the byte after it.
A lone escape byte only appears when a sequence is split across reads, so the loop waits up to 50 ms for the rest.
A bracketed paste is one event however many reads it spans.

## Screen

Each frame clears the back grid, draws everything, and `ScreenFlush` writes the cells that differ from the last frame.
A cell holds a whole grapheme cluster and a style, and a wide character's second cell has width 0.
After any non-ASCII cell the writer moves the cursor explicitly, so a terminal that disagrees about a width cannot shift the rest of the row.

Images are placed over a rectangle of cells for one frame (at most two, one per side of a split), with a key that tells pictures apart.
Writing over a sixel erases it, so image cells are never written unless the images changed, never used to fill a gap between changed cells, and an image something was drawn over (a picker) is left out.
When the images placed differ from those shown, every image cell is written as a space, which erases the old ones, and each image is sent again; otherwise no sixel is sent, so a frame that only changes text never resends one.
Images are kept clear of the bottom row, where foot would scroll the screen to fit them.

## Text and files

Bytes are stored exactly as read.
An invalid byte is one U+FFFD wherever text is decoded, so invalid UTF-8 survives editing and saving.
Cursor movement steps over grapheme clusters by the extended rules, except the Indic conjunct rule (GB9c), which would need another table.
A cluster is as wide as its first code point, except that a flag and a pictographic character before U+FE0F are two columns.
Display columns are always worked out from the bytes, never stored.

`FileRead` refuses anything that is not a regular file, files over `maxFileSize` and files with a nul byte in the first `binaryProbeSize` bytes, and opens with `O_NONBLOCK` so a named pipe is refused rather than hanging.
Loading turns every `\r\n` into `\n` and records the more common ending, which a save writes back, so a file with one ending round trips byte for byte; a lone `\r` is content, shown as `^M`.

`FileSave` writes a temporary file `.<name>.editor-XXXXXX` in the target's folder, sets its permissions, `fsync`s it and renames it over the target, so a save is atomic.
Symbolic links are resolved first and a broken one is refused.
An existing file keeps its permissions and a new one gets `0666` less the umask; ownership, extended attributes and hard links are not kept, since the rename makes a new inode.
The folder walk skips the temporary files a save cut short leaves behind.
Before writing, the target is compared with the stamp of the last load or save, and a file changed on disk is only overwritten when the user confirms.
The change time is in the stamp, so a rewrite of the same size whose modification time was set back is still caught.

## Views

A `ViewRow` is a line plus the index of one of its wrapped rows, and `ViewStep` moves by any number of visual rows.
Vertical movement, paging, scrolling and the scroll margin are all written against it, so the same code serves wrapped and unwrapped views.
Nothing holds a row index for the whole buffer: a wrapped line is laid out when it is reached, so the cost follows the rows stepped over, and without soft wrap stepping is arithmetic on line numbers.
A wrapped line of 4 KB or more keeps its rows in a small cache in the view (four lines, emptied by any change), since scrolling and drawing ask for the same line many times a frame.

Columns depend on everything before them on the line, so measuring one starts at the line's beginning; `layout` passes over runs of printable ASCII a byte at a time, which makes long lines several times cheaper.
A wrapped row holds at most one column less than the text width, so the end of a line always has a cell on its last row for the cursor.
Drawing starts at the first visible row, or without soft wrap at the cluster under the first visible column, so the part of a long line before the view is never laid out cluster by cluster.

`ViewScrollToCursor` runs before every frame, so every command ends with the cursors in sight.
It keeps `scrollOffset` rows and columns of margin, capped under half the height, and with several cursors keeps all their heads in sight when they fit.

The status bar shows the name with `[+]` and `[RO]`, then on the right the fields that differ from the defaults, with `line:column` last so it stays put as others come and go.
With more than one cursor every cursor is drawn by restyling its cell after the text, and the terminal cursor is hidden.

### Mouse

`app` routes mouse events against the frame the user saw: `render` records the rectangle of each pane and status bar it drew, and a press is judged against those, never against a view pointer.
A scan, learning mode and prompts leave the mouse alone, an open picker takes presses and the wheel on its list, and otherwise the panes get them.
`ViewPositionAtScreen` turns a cell into a position by stepping from the view's top row, so it agrees with drawing on tabs, wide clusters and wrapped rows.
`mouse.h` describes presses, double and triple clicks, drags and column drags.
A press remembers a plain `Position`, which does not follow edits, so any event other than the mouse ends a drag and forgets a click count.

## Images

`DocumentOpen` loads every file as text first; when `FileLoad` refuses one as binary, `ImageOpen` reads it with `FileReadBytes`, recognises PNG, GIF, BMP or JPEG by its first bytes, and decodes it, or gives the decoder's reason.
An image of more than `maxImagePixels` is refused from its header, before anything is allocated.
A binary file is read twice this way, which costs a few milliseconds from the page cache against probing every file before loading it.
The decoders free large buffers as soon as they are done with them, and `ImageOpen` then calls `malloc_trim`, since glibc otherwise keeps the freed memory.

`png` inflates the joined `IDAT` chunks to exactly the size the header gives, so a stream that would inflate to more is refused rather than filling memory.
`jpeg` decodes every scan into coefficients, so baseline and progressive images share everything after entropy decoding, and frees each component's coefficients once its samples are made.
Each decoder's header lists what it reads and refuses.

`ImageRender` draws an image at its own size when it fits, or else shrunk (averaging the pixels each one covers, weighted by alpha) to the largest size of the same shape that does, with five pixels spare for foot's six-pixel sixel bands.
An image with transparency is first laid over a dark checkerboard, so every pixel is drawn opaque.
`sixel` picks a palette of 255 by median cut over 5-bit bins and always applies Floyd-Steinberg diffusion: a bin holding one exact colour gets that colour, so an image of few colours stays crisp, while greyscale and gradients are dithered rather than banded.
Each image keeps its renderings for the last two areas drawn, so the two sides of a split do not encode it again each frame.
At its actual size (Alt-0) the part shown belongs to the image, so both sides of a split show the same part, and the movement keys pan it before the view-only check refuses them.

## Editing

**Cursors follow changes.** A view moves each cursor with `BufferAdjust` after every replace: a position before the change stays, one inside the replaced text goes to its start, and one after moves with the text.
An edit command loops over the cursors, replacing each one's range, and merges the cursors that overlap at the end.
Line commands work on the runs of lines the cursors touch, taken together, and moving lines refuses as a whole when any run is at the edge of the buffer.
Ctrl-K, joining, sorting and the commands that place one cursor first collapse to the primary cursor, which is the one most recently added.
An edit's cost grows with the square of the cursor count, since each replace adjusts every other cursor.

**Paste.** A paste with as many lines as there are cursors gives each cursor one line, in buffer order.
Copy with no selection is line wise, and pasting exactly that text again inserts it as whole lines above the cursor's line.
A paste has `\r\n` turned into `\n` and is one undo step without auto indentation.

**Typing.** Enter copies the indentation and adds a level after `{`, `[` or `(`, opening a block between a pair.
A closer typed at the end of a line holding only blanks takes a level off it, in the same undo group.
A bracket or quote typed over a selection surrounds it, as an undo group of its own kind.

**Case conversion** maps code point by code point with Unicode's simple mappings, which are one to one, so the editing view's cursors are remembered as code point counts and turned back into bytes after a mapping changes a character's length.

**Comments.** Ctrl-/ decides once for every touched line: when each line that is not blank is commented, it uncomments, otherwise it comments, with the markers lined up at the smallest indentation of each block.
It runs as line edits like indentation.

**Markdown tables.** Alt-Shift-G asks `highlight` for the `pipe_table` node around the cursor (`HighlightNodeOfType`, so `highlight` still knows nothing of Markdown), and the grammar decides what is a table.
A body cell wider than the width asked for wraps onto lines added below its row; Markdown has no multi-line cells, so a second alignment sees those as rows of their own and cannot join them up.
Header cells never wrap, since a line added under the header would end the table's head.

**Changes across files.** `multifile` makes a project replace or a server's rename, fix or formatting one thing that can be listed and taken back.
Each file's part is one undo group, whose id it keeps with the entry's serial; no text is kept, since the groups hold it.
A file's state is read from its history each time: gone, the last step, done with steps after it, the next to redo, or further back.
The list undoes or redoes only the files whose group is at the edge of their history, since anything else would take later edits with it.
The places are worked out from the groups when the list opens, moved through every later change, so they stay right after later edits.

**Prompts** edit one line in the message bar, or wait for one of a few letters.
The prompt closes before its callback runs, so a callback can open the next one.
A prompt can take a change handler (pickers filter with it) and a completion handler (Tab in path prompts).

**Learning mode** takes every key while it is on, ahead of pickers and prompts, so no command runs.
It asks the bindings held with Ctrl or Alt in a shuffled order and gives the answer after three misses.

**Clipboard.** Copy runs `wl-copy` and paste `wl-paste --no-newline --type text` with `posix_spawnp`, standard error discarded and a time limit (two seconds to copy, half a second to paste), after which the command is killed.
SIGPIPE is ignored so a command that exits early cannot end the editor.
Paste falls back on the editor's own copy only without `WAYLAND_DISPLAY` or when `wl-paste` cannot start, and says so; a `wl-paste` that fails or runs out of time pastes nothing, rather than a copy that may be stale.

**Save flow.** `save` runs a queue: the current document for Ctrl-S, every modified one for Ctrl-Shift-S and before quitting or replacing the buffers.
Each is trimmed when `trimTrailingWhitespace` is set, then written, and the next starts from the completion of the last.
A buffer without a name asks for one, a name that exists asks before overwriting, and a file changed on disk asks again.
Any error, refusal or cancel stops the queue, and the action waiting for it (quit, close, replace) never runs.
The Alt-X menu remembers the entry's serial before asking, so its answers never act on another buffer.
A rename across file systems copies into a temporary file next to the target and removes the original.

**Recovery.** Once input has rested `recoverDelay` (2 s), the main loop copies every document changed since the last pass to `<state folder>/editor/recover`, made `0700`.
A copy is named after the file's absolute path with `/` as `%` (or `unnamed-N`), then `~` and the pid, so Ctrl-O shows what each one was and two sessions never share one.
It goes through `FileSave`, so it is atomic.
A failed copy is tried again only after the next change, so a full disk does not wake the loop over and over.
A buffer that is saved, undone to its save point, closed or discarded loses its copy, and a clean exit removes the process's copies.
On a hang-up every modified buffer is copied whatever its size (over `maxRecoverSize` only then), and the editor exits normally.
Crash signals and `Fatal` write nothing (`FileSave` allocates, and the heap may be damaged), so they rely on the last idle copy.
At startup `RecoverReport` counts the copies whose pid no longer runs; the user opens them with Ctrl-O and deletes them.

**Split view.** The focused side's entry is the workspace's current entry, so every command works on the focused view without knowing about the split.
The workspace also keeps the other side's entry and which slot is on the left, so Ctrl-I only flips that.
A document's `view` points to the slot that last had focus on it; when a side shows a document last seen in the other slot, it copies that view's cursors and scroll position first, so a buffer opens where it was last seen.
Both views stay listeners for the document's lifetime, but undo restores the cursors only in the view that undoes.
Below `minSplitWidth` columns only the focused view is drawn and nothing about the split changes, so widening the window brings both back.

## Search

One pattern syntax serves Ctrl-F, Ctrl-R, Alt-N, Alt-P, project search and Ctrl-E.
A pattern is literal unless its options say regular expression, `^` and `$` at its ends are anchors, and a match never crosses a line break and is never empty, so stepping through matches always moves.
Without case matching (smart case: no upper case letter outside an escape, unless Ctrl-T asks for case always) the pattern is kept folded and the text folded as it is compared, so offsets are always those of the original text.
A match must start and end on grapheme cluster boundaries.

Every matcher goes through `PatternFind` and `PatternReplaceLine`, so highlighting, stepping, replace and project search take regular expressions with no code of their own.
The VM keeps its threads in priority order, so it finds the leftmost match and among those the one Perl would, except where a repetition's body can match empty: there it goes on, as RE2 does.
When every match starts with one ASCII byte it skips ahead with `memchr`.
Group positions are only worked out when a replacement names a group, by running the VM again anchored at the match.
Programs are capped at 4096 instructions and groups at 1000 deep, since each level is a level of recursion.

Rendering looks for matches only up to 64 KB past the cluster being drawn (`matchReach`), so a 17 MB line is searched as far as it is shown.
A match that is also selected is drawn in a stronger colour, which is how the current match stands out.
Incremental find saves the cursors, scroll position, match kind and pattern, and puts them back on Esc.
Typing on after a literal pattern stopped matching skips the search, since adding to it cannot match.
The search prompts share a session history of `searchHistorySize` patterns, and the replacement prompts one of replacements.
Up or Down in an empty prompt recalls the newest entry, and they browse the history only while the prompt holds the entry recalled, unchanged; otherwise they step between matches.
The search prompts take Ctrl-T and Ctrl-R before the prompt sees them, and the replacement prompts leave them alone.

**Replace.** Ctrl-R runs from the selection to the end of the buffer and round to where it started, or within a selection of more than one line.
The stop moves with each replacement through `BufferAdjust`, and every search continues after the last replacement, so replacing `a` with `aa` cannot loop.
The run is one undo group; replacing all rebuilds each line once.

**Spell check.** The word list is a `Buffer` loaded from `words.txt` the first time it is needed, and again when its stamp changes, sorted in memory when edited by hand out of order, and binary searched in byte order.
Adding or removing a word edits the buffer and saves the list with `FileSave`, undoing the change in memory when the save fails.
In a highlighted buffer, spell check only looks at the text `HighlightSpellable` allows (see Highlighting); other buffers are checked in full.

**Scans.** A client gives `scan` the entry indexes and four functions: an optional `wants`, which decides from a file's stamp whether to read it; `visit`, which takes an open document or the text read from disk; `progress`; and `finish`.
`ScanContinue` visits for about 20 ms, then returns so the loop can draw and read keys.
One text buffer is reused for every file read.
The scan is over before `finish` runs, so a client may start another.

**Project search** is a scan.
Open entries are searched in memory and the others read from disk without being opened, with `\r` before a line break left out, so line numbers match what opening gives.
Each matching line is recorded once, with its first match, up to 200 bytes of its text, the entry's serial and the buffer revision it was found at (or that it came from disk).
Alt-F searches again the listed entries that are open and changed, so lines that no longer match drop out and line numbers follow inserted lines.
The list leaves out hits whose serial no longer matches and counts them.

**Project replace** works on the lines the filtered list shows.
Each entry is opened through `MultifileEditBegin`, and its lines are replaced from the last up, each matched again in the current text, so a line edited since the search only takes the matches it still has.
An entry read from disk whose file has changed since, on disk or by an edit after opening, is left alone and counted, since its line numbers may no longer lead to the lines listed.
Nothing is saved.

## Buffers

**Loading.** `loader` walks folders depth first with `openat` and `fdopendir`, each listing sorted by name.
Entry types come from `d_type`, so a regular file costs no `stat`, and its id is the folder's device and the entry's inode.
A link is followed only to a regular file.
Pipes, sockets, devices, broken links, `.git` folders and leftover save files are skipped.
Files are de-duplicated by id, so a file reached twice appears once, under the first path that reached it.
The walk stops as soon as it would pass `maxFileCount`.
Nothing but listings and `.editor` files is read.

**.editor files.** Each walked folder is probed for one.
Its patterns replace those from above for that folder and everything below, matched relative to it; a folder is tested before it is entered, so a left-out folder is never read.
A file also matches a folder pattern that matches any folder above it, which project search scopes rely on.
Files named on the command line are always loaded.

**Entries.** An entry holds its path until it is opened, the file id, the document once open, a refusal, a number for an unnamed buffer and a "last used" tick.
A file refused as binary or too large is remembered and never read again; other errors are tried again next time.
`WorkspaceFind` takes an entry to be the same file when its id matches (as of its last load or save) or its path does, so a file that was saved or replaced is not loaded twice.
When the current entry goes, the most recently used open one takes its place, or else a new unnamed buffer, so a file the user never opened is not read.
Closing a folder removes its entries in one pass by serial and stops the servers no document shows.

**Names.** Entries keep the path they were loaded or saved by and are shown by a name: the open folder's name and the path inside it, or a path relative to the working folder, or with `~`.
Each entry caches its name with the generation of the folder list it was worked out for, and a rename or save as clears it.

**Placeholders.** The buffer picker may stand over a new unnamed buffer made only so something is current (after opening a folder, or closing the last open buffer).
Picking a buffer removes the placeholder unless the other view shows it or it was edited.

**Pickers** keep a case folded copy of each item, so filtering 100,000 paths is a byte search per item.
A picker takes every key; soft wrap is the one command `app` runs past it, so a preview and the editor share one wrap setting.
A list may take keys with modifiers first (project replace, the multi-file undo), and may be suspended while a question is asked and resumed as it was.

**Jump history** keeps 32 places by serial and position, which do not follow edits and are clamped when used.
A place in a closed entry, or where the cursor already is, is passed over.

## Highlighting

`LangLoadAll` reads every package's `language` file at startup, in order of id, into one allocation that lives until exit, so a `const Lang *` never dangles.
It loads no grammar.
A file's language comes from its whole name, then its extension, then its `#!` line, which the document reads once when it opens.
A `language` file with an unknown or repeated key, a bad value or a missing requirement leaves its package out, and the first problem is shown at startup.
The first file of a language opens its `parser.so` (`dlopen` with `RTLD_NOW | RTLD_LOCAL`, so grammars cannot clash), checks its ABI (13 to 15), and compiles `highlights.scm` and the optional `injections.scm`, shared by every buffer of the language.
A grammar that fails is reported once and its language left uncoloured.

**Parsing.** A highlighter listens to its buffer and turns each change into a `TSInputEdit` at once, but parses only when a line is asked for, so a burst of edits costs one parse.
The parser reads the buffer through a callback a line at a time, so the text is never copied into one string.
A parse has a deadline of `maxParseTime` of the thread's processor time, so a busy machine or a suspend does not count; one that runs past it turns highlighting off for the buffer until it is reloaded, and buffers over `maxHighlightSize` are never parsed.
Dropping the trees calls `malloc_trim`, since glibc otherwise keeps the freed memory.
The status bar shows the language as `(uncoloured)` in both cases.

**Injections.** An injected language comes from `#set! injection.language` or an `@injection.language` capture, looked up by injection id and then as an extension, so a fence can say `js`.
It covers its `@injection.content` node less the node's named children, which stay with the outer language.
Each injection is a layer, found again by grammar and first byte, so an unchanged code block is not parsed again.
Layers nest up to four deep, and beyond 512, those not used by the last two windows are dropped.
`injection.combined` is not supported, so each piece of PHP's HTML is parsed on its own.
Grammars marked `fresh-parse` (Markdown's inline grammar, whose scanner gives different trees when parsed incrementally) are parsed from scratch, a paragraph at a time.

### Colouring lines

`HighlightLine` gives one byte per text byte: the syntax group in the low six bits and two flags for spell check.
It fills a window of lines around the one asked for (enough for a screen; any edit invalidates it, and one window is kept per side of a split) by running each layer's query over just those rows.
Captures that fail their predicates are dropped, and the rest are painted in order of start, enclosing nodes before inner ones, and for the same node by pattern: an inner node beats the node around it and a later pattern beats an earlier one, as most vendored queries expect.
A package whose query puts its specific patterns first (Go's) sets `first-pattern-wins`, which reverses the pattern order for captures of the same node.
Injected layers are painted after the layer holding them.
A line over 16 KB is coloured only from 16 KB before the first byte drawn to 64 KB after it, each part's captures clipped to it, so minified code stays responsive.

A capture name picks the theme group that shares its first component (`string.special` is `string`), except that any `*.builtin` is `builtin` and Markdown's `text.*` captures use their second component (`title`, `literal`, `emphasis`, `strong`, `uri`, `reference`).
Some first components are aliases: `boolean` is `constant`, `character` is `string`, `conditional` and `storageclass` are `keyword`, `float` is `number` and `module` is `namespace`.
Containers (`embedded`), markers (`spell`, `error`), XML's `markup.*` and SQL's `field` and `parameter` are left uncoloured on purpose.
A capture with no group colours nothing, and `@none` clears the colour.

`query` evaluates `#eq?`, `#match?`, `#any-of?`, `#lua-match?` and their `#not-` forms against the captured text.
`#match?` patterns go to `RegexCompile` as they are, since the queries are written in the Rust dialect it follows; `#lua-match?` patterns (Zig) are rewritten into that syntax.
An unsupported predicate would let its pattern match every node, so a package author should check for them; `#is-not? local`, `#offset!` and `#set! priority` are ignored.

**Spell check scope.** Painting `literal` or `uri` sets a sticky literal flag, so a code block's contents stay marked as code under their injected colours, and `string.special` sets a special flag.
Spell check passes over both, and otherwise looks at comments and strings, and in prose languages also at plain text and markup.

### Symbols

A package may hold `tags.scm`.
The default packages' tags queries are the editor's own, since upstream's capture only part of a declaration.
The contract:
- `@name` captures the symbol's name.
- `@definition.<kind>` captures the whole declaration, where the kind is `function`, `prototype`, `type`, `struct`, `enum`, `macro` or `heading`.
- `@body`, optional, marks where the signature stops, for declarations whose grammar has no `body` field (a Go struct's field list, a setext heading's underline).

A symbol's signature is the declaration's text up to its first `body` field or `@body` node, with comments left out, blanks collapsed and cut to `maxSignatureLength`.
`HighlightSymbols` runs the query over the buffer's own tree (not injected layers), and `HighlightSymbolsOfText` does the same for a file that is not open, with a parser of its own under the same deadline.

The project symbol index keeps every symbol by name, with its entry's serial, kind and range.
Each indexed file records whether its symbols came from an open buffer at a revision or from disk at a stamp.
Before each lookup, open documents whose revision changed are indexed again from their live tree, so a function written and not saved is found.
Re-indexed files bump a generation instead of removing their old symbols, and a merge before the next lookup sorts in the new ones and drops the stale, so an edit costs one parse and a linear merge.
The index is built by a scan the first time a command needs it, and a name not found makes a second scan read only the files whose stamp changed.

### Syntax selection

`HighlightNodeAround` finds the smallest named node larger than a range, in the buffer's tree or an injected layer that is parsed and unedited.
Alt-Right pushes the cursors onto a stack in `move` and remembers the view, revision and cursors it left, and Alt-Left pops only while all three are unchanged.

## Git

`GitRepoOf` walks up from the file's real folder to the first `.git`.
A `.git` folder holding `objects` is a repository; a `.git` file (worktree or submodule) ends the walk with none, since the repository above does not track these files.
`core.bare`, reftable, an unknown object format or a format version over 1 refuse it.
Repositories are kept until exit, readable or not, so a `GitRepo *` never dangles.

`GitHead` follows `HEAD` through loose and packed refs and keeps their stamps, so a check costs three `stat` calls, and the refs are read again only after a commit, a checkout or a `gc`.
When they change, packs whose index is gone are unmapped.
An object is looked for in the mapped packs, then as a loose file, and when both miss the pack folder is listed again.
Delta chains are followed to a whole object (at most 10,000 deep) and applied from the bottom up, and the last 32 objects rebuilt (each up to 4 MB) are cached with each chain's base, so the trees above a file are not rebuilt for every file.
Sizes come from the repository, so none is trusted past `maxDiffSize` before memory is reserved for it.

**Changes.** A document gets its `Changes` when it opens with a path, and again after a save or rename gives it another one.
The committed text is read again only when HEAD names another commit, which is checked when the document is switched to, reloaded or saved, and when the panel or the picker opens.
Changes do not listen to the buffer: `ChangesHunks` works the hunks out again whenever it is asked after the revision changed, so commands always act on the text as it is.
What a frame draws keeps the hunks of an earlier revision until the buffer has rested `changesDelay` (150 ms), so typing in a large tracked file does not diff it after every key, and `ChangesTimeout` wakes the loop to draw the new marks.

`diff` passes over the common start and end, gives the lines in between ids from a hash table, and runs Myers' algorithm on the ids, dividing at the middle of a shortest edit script.
A part whose cost passes `maxDiffCost` is reported as one hunk, so unrelated texts take bounded time.
Its added and removed totals equal `git diff --minimal`'s over random edits, except that a file losing its final line break counts differently.

The change panel (Alt-G) and the diagnostics panel (Alt-K) draw into the right half of the window through a `FramePanel` function, the shape of a picker's preview; only one shows, and it takes the place of a split without changing it.
The Ctrl-G preview shows a whole file as a diff without building its rows: the row each hunk's old lines start on is the hunk's start plus the old lines before it, so a binary search tells what any row shows.

## Language servers

A package may name a `server` and the `server-roots` that mark a project's root (`.git` by default).
`lsp` keeps one server per language and root, at most eight.
When a document opens or changes path, it asks `lsp` for an `LspDocument`, much as it asks `highlight` for a highlighter: nothing when servers are off, the language has none, the buffer is unnamed or over `maxServerFileSize`.
Otherwise the root is the first folder above the file holding a root name, or else the folder the editor started in, and the server for it starts if none runs.

`rpc` starts the program with `posix_spawnp` in the root, with pipes for standard input and output (close on exec, so a later server does not hide an earlier one's exit) and standard error appended to `<state folder>/editor/<language id>.log`, which a start empties unless another server of the language is writing to it.
Both ends are non-blocking, and `RpcPump` writes what the pipe takes and handles each complete message.
A broken `Content-Length` header loses the framing for good, so it counts as the server's output ending.
Requests the server sends are answered at once: `workspace/configuration` with nulls, registrations and progress with null, `workspace/applyEdit` with `applied: false`, anything else with "method not found".

A server that ends, breaks its framing or fails a write is marked stopped, its documents stay attached and show `stopped`, and nothing restarts it behind the user's back: Alt-V does.
Quitting sends `shutdown` to each server, waits until all reply or `serverShutdownTime` passes, sends `exit`, and ends the stragglers with SIGTERM and then SIGKILL.
How a server understands the project (include paths, warnings) is its own configuration, kept in the project.

**Documents.** An `LspDocument` listens to its buffer and appends each change, as a range and new text, to a pending list, which `LspPump` sends as one `didChange` per pass of the loop.
Once more than `maxServerChanges` pile up, or a reload replaces the text, the whole text is sent instead.
Pending changes are sent before `didSave` and before every request, so the server never answers about text the user no longer sees.
Positions are bytes for a server that takes UTF-8, and UTF-16 code units otherwise, converted from the line's text.
Text goes as JSON strings, in which invalid UTF-8 becomes U+FFFD, so byte columns after an invalid byte differ by two for each.
Files are not watched: a reloaded buffer sends its whole text, and a header changed on disk reaches the server when it notices or a file including it is saved.

**Diagnostics.** A publication replaces a document's diagnostics, unless it is for older text (another version, or changes still waiting), since the server will publish again.
The `LspDocument`'s listener moves every range with `BufferAdjust`, so marks follow the text between publications.
Drawing a line finds the diagnostics that reach it with two binary searches, using the most lines any diagnostic spans, so a frame's cost follows the rows drawn, not the number of diagnostics.

**Requests.** The questions a command waits on go through `Ask`, which sends pending changes first.
It returns 0 at once when the server is not ready or lacks the capability, and the command falls back (Alt-D on the symbol index).
Otherwise the request has a deadline of `serverRequestTimeout`, after which it is cancelled and its callback runs with no answer; the status bar shows `waiting` meanwhile.
A command remembers the entry's serial, revision and cursor when it asks (a `WorkspaceSpot`), and drops an answer that finds any of them changed; formatting only checks the revision, since its edits would land in the wrong places.

**Edits.** Renames, fixes and formatting come back as text edits, and file operations in a `WorkspaceEdit` are counted and left out.
`refactor` applies them by path, from the last range up, with insertions at one position in reverse order, so each range is still where the server meant.
Each file is loaded if need be and opened through `MultifileEditBegin`, so its changes are one undo step and the whole can be undone at once.
Fixes are asked for with the diagnostics on the cursor's line sent back as the server sent them, since clangd finds its fixes by a diagnostic's range and message.
A fix that is only a command for the server to run is not offered, since `workspace/applyEdit` is refused.

## Performance

Measured in October 2026, as the review's fixes landed, on the release build on an idle 20-core x64 machine, in a 200×50 foot window, from the key to the end of the frame, as medians, with `.review/tools/bench.py`:

| case | figure |
| --- | --- |
| typing in a 1,400-line C file | 2.5 ms a key; 0.9 ms with no grammar |
| typing in a 34,000-line C file (1 MB) | 9 ms a key, mostly tree-sitter's incremental parse |
| typing in a git-tracked 10,000-line C file | 8.5 ms a key, the same as untracked, since the marks wait 150 ms |
| 100 MB file, 1.79 million lines | loads in 0.2 s; moving, typing and paging 1 ms, Enter 2.5 ms, undo 7 ms |
| indenting every line of it | 0.3 s, undone in 0.11 s, adding 210 MB |
| find on it, after the pattern stops matching | 1 ms a key (a full search is 0.1 s) |
| regex search of it matching nothing | 1.1 s for `\d{3}x`, 2.9 s for `\w+@` |
| 17 MB single line | 17 ms a key without wrap, End 19 ms; 75 ms with wrap after an edit |
| 3 MB Markdown file | opens in 1.1 s using 320 MB; typing 0.9 ms a key |
| 100,000 files | to the picker in 75 ms using 60 MB; filtering 3 ms a key |
| 1,000 cursors / 10,000 cursors | an edit takes 4–22 ms / 0.2 s |
| diff of a 15 MB file with its commit | 25 ms with one change, 85 ms with 330 |
| reading a file from a pack | 3 ms for a source file, 40 ms for a 4–17 MB `parser.c` (before inflate's lookup table) |
| 12 MP JPEG | 0.5 s, peaking at 85 MB |
| 3000×2000 16-bit PNG / 12 MP noise PNG | 0.5 s / 1.2 s, the latter peaking at 210 MB |
| indexing the symbols of `src/` | 0.5 s, resolving all 3,872 calls |
| binary / default packages | 0.8 MB / 8.1 MB |

Diagnostics on every visible row and a running clangd add nothing measurable to a keystroke.

## Conventions

STYLE.md covers how code in `src/` is written: naming, types, idioms, comments and layout.

- Vendored code is compiled with `-w` and not edited, apart from the patches in `vendor/patches/` (see The default packages).
  The tags queries and `language` files the editor needs live in `languages/`, outside `vendor/`.
- `src/` is C23, built with strict warnings (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wformat-nonliteral -Wjump-misses-init -Werror`).
  `compile_commands.json` carries the same flags except `-Wjump-misses-init`, which clang does not know, and `-Werror`.
- This document and README.md put each sentence on its own line, so diffs show the sentences that changed and nothing needs reflowing.

## The default packages

`./build.sh --languages` builds a package for every folder in `vendor/grammars/`: the grammar's `parser.c` and `scanner.c` compiled with `-fPIC` and linked as `parser.so`, the vendored `highlights.scm` and `injections.scm` (PHP's `injections.scm` and `injections-text.scm` joined into one), and the files in `languages/<id>/`.
`--install` copies each file next to its target and renames it into place, so an editor with the old `parser.so` loaded keeps its copy.
Grammar objects are rebuilt only when something in the grammar's folder is newer.
A cold build of the editor with every package takes about 3.5 seconds on 20 cores.

The packages take 8.1 MB: SQL 2.4 MB, Bash 1.4 MB, PHP 1.1 MB, Zig 0.7 MB, C 0.6 MB and the rest under 0.5 MB each.
Reading all fourteen `language` files at startup takes well under a millisecond.

The vendored versions:

| Component | Source | Version | ABI |
| --- | --- | --- | --- |
| tree-sitter runtime | github.com/tree-sitter/tree-sitter | v0.27.1 | 13 to 15 |
| C | github.com/tree-sitter/tree-sitter-c | v0.24.2 | 15 |
| HTML | github.com/tree-sitter/tree-sitter-html | v0.23.2 | 14 |
| CSS | github.com/tree-sitter/tree-sitter-css | v0.25.0 | 15 |
| JSON | github.com/tree-sitter/tree-sitter-json | v0.24.8 | 14 |
| JavaScript | github.com/tree-sitter/tree-sitter-javascript | v0.25.0 | 15 |
| Markdown (block and inline) | github.com/tree-sitter-grammars/tree-sitter-markdown | v0.5.3 | 15 |
| Bash | github.com/tree-sitter/tree-sitter-bash | v0.25.1 | 15 |
| Go | github.com/tree-sitter/tree-sitter-go | v0.25.0 | 15 |
| PHP (the `php` grammar, which embeds HTML) | github.com/tree-sitter/tree-sitter-php | v0.25.1 | 15 |
| SQL | github.com/DerekStride/tree-sitter-sql, from the npm package `@derekstride/tree-sitter-sql` | v0.3.11 | 14 |
| XML (without DTD) | github.com/tree-sitter-grammars/tree-sitter-xml | v0.7.0 | 14 |
| YAML (core schema) | github.com/tree-sitter-grammars/tree-sitter-yaml | v0.7.2 | 14 |
| Zig | github.com/tree-sitter-grammars/tree-sitter-zig | v1.1.2 | 14 |

Two scanners are patched, the one exception to leaving vendored code as upstream wrote it.
Their `serialize` functions wrote past tree-sitter's 1024-byte state buffer: Markdown's copied every open block (4 bytes each) with no bound, and YAML's loop allowed 2 bytes past the end.
The runtime then fails an assertion and aborts.
About 255 nested quotes or list items in a Markdown file, or 260 indent levels in a YAML file, was enough, and the symbol index parses every such file in the open folders, so the file need not be opened.
The patches stop writing when the buffer is full, so nesting deeper than that is coloured wrongly instead.
Each is kept as a diff in `vendor/patches/` (`markdown-serialize.diff`, `yaml-serialize.diff`) to apply again with `git apply` after updating the grammar, unless upstream has fixed it.

Each vendored folder keeps its upstream `LICENSE`.
The SQL repository does not commit its generated `parser.c`, so `parser.c`, `scanner.c` and `highlights.scm` come from the npm package of the same version.
Zig's `injections.scm` is left out: it only injects a `comment` grammar the editor does not have.
The runtime's WebAssembly support is left out; `wasm_store.c` is kept because `lib.c` includes it, but it compiles to nothing without `TREE_SITTER_FEATURE_WASM`.

## Unicode tables

`src/unicodedata.c` is generated and committed.
To regenerate it, for example for a new Unicode version:

1. Download `UnicodeData.txt`, `EastAsianWidth.txt`, `CaseFolding.txt`, `auxiliary/GraphemeBreakProperty.txt` and `emoji/emoji-data.txt` from `https://www.unicode.org/Public/<version>/ucd/` into one folder.
2. Run `tools/unicodedata.py <folder> > src/unicodedata.c`.

Case folding (for search, sort and pickers) uses the `C` and `S` entries of `CaseFolding.txt`, and case conversion uses the simple upper and lower case mappings in fields 12 and 13 of `UnicodeData.txt`.
The full and context dependent mappings of `SpecialCasing.txt` (`ß` to `SS`, final sigma, Turkish and Lithuanian rules) are not used.
The current tables are Unicode 16.0.0.
Width rules: East Asian Wide and Fullwidth are 2 columns.
Nonspacing marks, enclosing marks, format characters (except the soft hyphen), Hangul medial vowels and final consonants, and U+200B are 0.
Everything else is 1.
