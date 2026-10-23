# editor

A personal terminal text editor for Linux, written in C23.

It runs in the [foot](https://codeberg.org/dnkl/foot) terminal on Wayland, and nowhere else. See Foot.

## Build

```sh
./build.sh                         # optimised build, produces ./editor
./build.sh --debug                 # unoptimised build with address and undefined behaviour sanitizers
./build.sh --languages --install   # also builds the language packages and installs them and the word list
```

Requirements: gcc 15 or newer (C23), bash, and at runtime foot and `wl-clipboard` for system clipboard support. Optionally, a language server for the languages that name one (`clangd` for C, see Language servers).

The build compiles the vendored tree-sitter runtime once into `build/vendor` and only recompiles it when it changes. The editor sources are rebuilt every time. It also writes `compile_commands.json`, the flags each source is compiled with, for clangd (see Language servers).

Languages are not built into the editor: each one is a package in `~/.config/editor/languages/<id>/` (or under `$XDG_CONFIG_HOME`), read when the editor starts. `--languages` builds a package for each vendored grammar into `build/languages` (compiling a grammar again only when its sources change), and `--install` copies them to the config folder, replacing packages of the same name and leaving any others alone. Until they are installed, nothing is highlighted, Ctrl-/ does not know any comment markers and there are no symbols. Run `./build.sh --languages --install` again after updating the vendored grammars or queries; an editor already running keeps the packages it started with.

The spell check word list is not built in either: it is `~/.config/editor/words.txt` (or under `$XDG_CONFIG_HOME`), one word per line, and spell check is off until it exists. `--install` copies `data/words.txt` there when there is no list yet, and never replaces one, so words you add are kept. Any list of one word per line works (such as `/usr/share/dict/words`), in any order.

## Usage

```sh
editor                       # new empty buffer
editor file                  # open file (created on first save if it does not exist)
editor file:120[:8]          # open file at line 120 (and column 8), as compilers print it
editor file1 path/to/file2   # load both and pick one
editor folder                # load every file under folder and pick one (Escape: empty buffer)
editor file1 folder1 ...     # any mix of files and folders
```

A `path:line` or `path:line:column` (a trailing `:` is allowed) is split only when the whole name does not exist but the path does, so a file whose name has colons in it still opens as itself. Each file named with a line is opened at once, with its cursor there.

Folders are walked at any depth. `.git` folders, links to folders and the temporary files a save cut short leaves behind (`.<name>.editor-XXXXXX`) are skipped, and links to files are followed. A file reached twice (named again, inside a named folder, or through a link) is loaded once. Loaded files are only read when first used. More than 100,000 files is refused before the editor starts.

A `.editor` file in a folder leaves files out of that folder and everything below it, until a deeper `.editor` file replaces it. One pattern per line, and `#` starts a comment:

```
foo.txt       # files with this name at any depth
*.c           # files ending in .c at any depth
bar/          # folders with this name (and everything in them) at any depth
/foo/foo.txt  # this path, relative to the .editor file
/*.c          # .c files next to the .editor file
/bar/         # this folder, relative to the .editor file
```

`*` is the only wildcard and stays within one name. Files named on the command line are always loaded. `.editor` files are read when loading (at startup and when Ctrl-O opens a folder), not when they change.

Files over 100 MB, files with a nul byte in their first 4 KB, and anything that is not a regular file are not opened. They stay in the buffer list, marked in the pickers. A file is saved with the line ending it was loaded with (LF or CRLF); a file that mixes both is normalised to the more common one. Saving is atomic, follows symbolic links and keeps the file's permissions. If the file changed on disk since it was opened, the editor asks before overwriting it.

## Features

- Movement: the arrows, Home (first non-blank, then line start), End, PageUp, PageDown, Ctrl-Left and Ctrl-Right (by word), Ctrl-Home and Ctrl-End, Alt-Up and Alt-Down (scroll a row, as the wheel does: the cursors stay put until they would reach the scroll margin, and are then carried along). Shift with any of them extends the selection. Ctrl-A selects everything, Ctrl-M selects inside the closest brackets (again to include the brackets, again for the next pair out), Alt-M jumps to the matching bracket, Ctrl-L goes to a line or `line:column`. Alt-Right grows every selection to the enclosing syntax node (a word, then the expression, statement, block, function and so on), and Alt-Left takes it back a step at a time, as long as nothing else moved the cursors in between; without syntax highlighting Alt-Right selects inside brackets as Ctrl-M does. Ctrl-Shift-Up and Ctrl-Shift-Down grow every selection by a line above or below, and Alt-Shift-Down and Alt-Shift-Up shrink it by a line at the top or bottom. The top of a selection moves between line starts and the bottom between line ends, so after Ctrl-M inside a function, Alt-Shift-Down and Alt-Shift-Up leave just the lines between the braces. Esc collapses several cursors to one, then clears the selection, then stops highlighting search matches or unknown words.
- Mouse: a click puts the cursor on the text clicked (past the end of a line at its end, in the line numbers at the start of the line), and a click on the other side of a split moves the focus there first. Dragging selects the text passed over, in either direction, without the line numbers. A double click selects a word and a triple click the whole line, and dragging after either grows the selection a word or a line at a time. Dragging to the top or bottom edge of the view (or onto the bars below it) scrolls it while the pointer keeps moving. Any key ends a drag. A click on the line numbers selects that line, and dragging there selects whole lines. A click on the status bar of the other side of a split moves the focus there. In a list (Ctrl-P, Ctrl-H and the rest), a click selects an item and a second click picks it, and the wheel moves through the list; over the preview of the changes list (Ctrl-G) it scrolls the preview. Alt-click and Alt-drag add cursors (see Multiple cursors). The wheel scrolls the side of the window under the pointer by three rows, without moving the focus. The cursors stay where they are until they would reach the scroll margin, and are then carried along, dropping their selections.
- Editing: typing (an opening bracket or a quote, `(`, `[`, `{`, `"`, `'` or `` ` ``, typed over a selection puts the pair around it instead, leaving the text inside selected, as one undo step), Enter (keeps the indentation, adds a level after `{`, `[` or `(`, and opens a block between a pair; a `}`, `]` or `)` typed at the end of a line holding only blanks takes a level off it), Tab (spaces up to the next tab stop, or indents every selected line), Shift-Tab (dedents the line or the selected lines), Backspace (deletes back to the previous tab stop in leading spaces), Delete, Ctrl-Backspace (previous word), Ctrl-Delete (next word), Ctrl-Enter and Ctrl-Shift-Enter (open a line below or above the cursor's line, wherever the cursor is on it, indented as Enter at its end would, or as the line is), Ctrl-D (duplicate lines), Ctrl-K (delete lines), Ctrl-Up and Ctrl-Down (move lines), Alt-J (join the selected lines, or the line and the next; each line break and the next line's leading blanks become one space, or nothing when either side is empty, the line ends in a blank or the next line starts with a closing bracket), Ctrl-T (trim trailing whitespace from the lines the cursors and selections touch), Ctrl-Shift-T (trim it from every line).
- Case: Alt-Shift-U converts every selection to upper case, and Alt-Shift-L to lower case. They need a selection: without one they say so and change nothing. They use Unicode's simple one to one mappings, so `ß` stays `ß` (not `SS`), capital sigma always becomes `σ` (never the final `ς`), and `I` becomes `i` in every language. Selections stay selected and cursors stay on the same characters, and each conversion is one undo step.
- Sort: Alt-O sorts the lines the selection touches and selects them. It needs a selection: without one it says so and changes nothing (Ctrl-A first sorts the whole buffer). It asks how (`sort: (a)sc (d)esc (u)nique (s)hortest (l)ongest; capital ignores case`): `a` ascending (as `LC_ALL=C sort`), `d` descending (as `sort -r`), `u` unique, ascending with one of each duplicate line kept (as `sort -u`), `s` shortest first or `l` longest first. Length is the width on screen: a tab counts up to its tab stop and a wide character as two columns. Lines of the same length stay in ascending order. The capital letters `A`, `D`, `U`, `S` and `L` do the same ignoring case: they compare simply case folded characters, lines that are then equal go in byte order (reversed by `D`), and `U` keeps the first in byte order of lines that differ only in case. It is one undo step. Cursors of the other view of a split inside the sorted lines move to their start.
- Markdown tables: Alt-Shift-G aligns the columns of the table the cursor is in, and says so when it is not in one (a table in a code block is not one). It asks for the widest a column may grow (`table column width (3-10000): `), starting from the last width given, or 50, selected so that typing replaces it. Each column is then as wide as its widest cell up to that width, with the text left aligned and padded with spaces, one space inside each pipe. The delimiter row keeps its alignment colons (`:---`, `:---:`, `---:`), and every column is at least three wide so they fit. A body cell wider than the width wraps at blanks (a longer word breaks between characters, never inside an escape such as `\|`) onto lines added below its row, with the other cells empty. Markdown has no cells over several lines, so each added line is a row of its own in the rendered table, and aligning again cannot join them back up: undo can, and a large width avoids wrapping at all. Header cells never wrap, as a line added under the header would end the table's head, so a column is at least as wide as its header, and its body cells wrap to that width when it is wider. Rows with fewer cells than the widest row get empty ones, and every row takes the header's indentation and quote markers (`> `). The table is one undo step, and the cursor goes to the start of the cell it was in. It finds tables through the syntax tree, so it needs Markdown highlighting (not for files over 16 MB).
- Comments: Ctrl-/ comments out every line touched by a cursor or selection, or uncomments them when they are all commented already. The markers line up at the smallest indentation of each block, and blank lines are left alone. The markers come from the language's package: in the default packages C, Go, JavaScript, PHP and Zig use `//`, Bash and YAML `#`, SQL `--`, CSS `/* */`, and HTML, Markdown and XML `<!-- -->`. JSON has no comments.
- Multiple cursors: Ctrl-E selects the word under the cursor, and each further press adds a cursor at the next occurrence of the selection (matching case, and as a whole word when the selection is one), going round the end of the file. Alt-E skips an occurrence: it moves the newest cursor on to the next one instead of adding a cursor. Ctrl-Shift-E adds a cursor at every occurrence at once. When the selection is a highlighted search match, the occurrences are the search's matches, so after Ctrl-F with the regular expression `Document\b` and Enter, Ctrl-E steps through the matches and Ctrl-Shift-E selects them all; after Alt-C they are the unknown words. Ctrl-Alt-Up and Ctrl-Alt-Down add a cursor on the row above the first cursor or below the last one. Alt-click adds a cursor where it is clicked, and Alt-click on a cursor (or inside its selection) removes it again, unless it is the last one. Alt-drag adds a cursor with a selection, and an Alt double click adds a cursor on a word. Ctrl-Alt-drag selects a column: every row from the press to the pointer gets a cursor selecting the same columns, and a row too short for them gets a cursor at its end. A second Alt-click on the same spot within 400 ms is a double click, so to remove a cursor just added, click it again a little later. Typing, Enter, Tab, Shift-Tab, the deletes, movement and Shift selection act at every cursor, and each edit is one undo step. Cursors that meet merge. Ctrl-D duplicates, and Ctrl-Up and Ctrl-Down move, the lines of every cursor, taking the lines of cursors next to each other together; moving does nothing when any of them is already at the start or end of the file. Ctrl-K, joining, sorting, Ctrl-L, Ctrl-M, Alt-M and the search commands first collapse to the newest cursor. With more than one cursor, every cursor is drawn as a block and the status bar shows how many there are (`3 cursors (esc)`, as Esc goes back to one).
- Aligning: Alt-= lines up the cursors in a column, inserting the fewest spaces before each that bring it level with the one furthest right, so with a cursor before each field name of a struct the names line up. A selection counts from its start and moves along with its text, so selecting the `=` of each assignment with Ctrl-E and pressing Alt-= lines up the `=` signs. With several cursors on a line, the first cursor of each line is one column, the second another and so on, aligned from the left, so a name and the comment after it line up in one go; a column is only aligned where it has cursors on two or more lines. Columns are measured as shown, with a tab reaching its tab stop and a wide character two columns, and the padding is always spaces. It is one undo step. It needs cursors on two or more lines, and says so when there are not, or when they line up already.
- Undo and redo: Ctrl-Z and Ctrl-Y. Consecutive typing, backspaces or deletes are one step until the cursor moves or another command runs. A paste is one step. Undoing back to the saved text clears `[+]`.
- Changes across files: a project replace, or a rename, fix or formatting from a language server, changes each file as one undo step, so Ctrl-Z in a buffer undoes only that file's part, and says so while the other files still have theirs. Alt-Shift-Z lists the places the last such change made, as `file:line` with the line as it is now, and a preview that picks out the new text; Enter goes to one. In the list, Ctrl-Z undoes the change in every file at once and Ctrl-Y redoes it, and the message says in how many files. A file edited since the change, or closed or reloaded, is left as it is and named in the message: undoing the change there would undo the later edits too, so that is left to Ctrl-Z in that buffer. Once undone, the list shows the old text back in its places, until an edit in a file drops its redo.
- Clipboard: Ctrl-C, Ctrl-X and Ctrl-V use `wl-copy` and `wl-paste`, and fall back to an in-editor clipboard without a Wayland session or `wl-paste` (a paste from it says so). A clipboard holding no text, such as an image, pastes nothing, and so does a `wl-paste` that has not answered in half a second. In a prompt or a list's filter, Ctrl-V pastes the clipboard's first line. With no selection, copy and cut take the whole line, and pasting that text again inserts it as a line above the cursor. With several cursors, copy joins the selections (or the cursors' lines) with line breaks, and a paste with as many lines as there are cursors gives each cursor one line. Pasted `\r\n` becomes `\n`.
- Files: Ctrl-S saves (asking for a name when the buffer has none, and before overwriting an existing file or one changed on disk), Alt-R reloads from disk (asking first if there are unsaved changes), Alt-Shift-R loads the files added to the open folder since it was read (by another program; files already found once, including closed ones, are not added again, and nothing else changes), Alt-; switches between LF and CRLF, Alt-Shift-X copies the file's path (as it was loaded or saved, rather than the name the status bar shows), and Ctrl-Q quits (offering to save unsaved changes). When the window regains focus, and when switching to a buffer, a buffer whose file another program changed since it was loaded or saved is reloaded if it has no unsaved changes (its undo history goes with the old text), and the message bar says so; one with unsaved changes is only noted (Alt-R reloads it).
- Recovery: unsaved changes are copied to `~/.local/state/editor/recover` (under `$XDG_STATE_HOME` when it is set) once typing stops for 2 seconds, and at once when the window closes or the editor gets SIGHUP or SIGTERM, after which it exits. A crash or SIGKILL loses at most the last 2 seconds of typing. A buffer over 16 MB is only copied when the window closes. Each copy is named after the file's full path with `/` as `%` (or `unnamed-N`), then `~` and the editor's process id. Saving a buffer, closing it, discarding its changes or quitting with Ctrl-Q removes its copy, so the folder only holds work that was lost, and the next start says how many copies earlier sessions left there. Open them with Ctrl-O, save what you want to keep and delete the rest.
- File menu: Alt-X asks for `r` rename, `c` copy or `d` delete. Rename and copy ask for the new path, starting from the current one (Tab completes, `~` is the home folder), and ask before replacing a file that exists. Rename moves the file on disk (copying it, with its permissions, when the new path is on another file system) and keeps unsaved changes unsaved; a new extension changes the language. Copy writes the buffer's text as it is (with its line ending, untrimmed) to the new path and leaves the buffer, its name and `[+]` alone. Delete asks first, then removes the file and closes its buffer. Unnamed buffers can be copied but not renamed or deleted.
- Images: a PNG, JPEG, GIF or BMP file opens as a picture instead of being refused as binary. It is drawn at its own size in the middle of the window when it fits, or else shrunk until its longest edge fits, keeping its shape, and drawn again at the new size when the window is resized or split. The status bar shows its format and size (`PNG 3000×2000`), and how much it is shrunk (`37%`). Images are view only: typing, pasting, saving and every other editing command say "images are view only", and the mouse only moves the focus. Alt-R reads the image again, and Alt-X renames, copies (the file as it is on disk) or deletes it. PNG is read in every colour type and bit depth, with transparency and interlacing; GIF shows the first frame of an animation, with its transparency; BMP is read at every bit depth, with bit fields, alpha and run-length compression; JPEG is read baseline or progressive, grey or colour, and turned upright as its EXIF orientation says (arithmetic coded, lossless, 12-bit and CMYK JPEGs are not opened). Alt-0 switches between that and the image's actual size, cut to the window: then the arrows move the part shown by a cell, PgUp and PgDn by a page, Home and End to the left and right edges, Ctrl-Home and Ctrl-End to the corners, and the wheel up and down, and the status bar shows `100%`. Transparent parts show over a dark checkerboard, and colours are reduced to 255 for the terminal. Alt-I gives the format, size, what the pixels are and the file's size. Images of more than 40 megapixels, damaged files and other binary files are not opened.
- Navigation: Alt-[ goes back to where the cursor was before a jump, and Alt-] forward again. Jumps are Ctrl-L, Ctrl-Home and Ctrl-End, opening a project search match, the symbol commands below, the language server commands (definitions, uses, the other file of a header or source) and going to a diagnostic; 32 places are kept, across buffers, and a place in a closed buffer is skipped. Jumping after going back drops the places ahead, as undo does. Alt-S lists the symbols of the file in order (functions, prototypes, types, structs, enums and macros in C, the equivalents in Go, JavaScript, PHP and Zig, Bash functions and Markdown headings), with the symbol named by the word at the cursor selected (so on a call, Enter goes to the function's definition rather than its prototype), or else the one the cursor is in. Each symbol's line number follows it, or in a window at least 80 columns wide a preview beside the list shows the lines around the selected one with its name picked out. Typing filters them, and Enter goes to its name. Ctrl-] and Ctrl-[ go to the start of the next or previous definition. Languages without symbols (CSS, HTML, JSON, SQL, XML, YAML), and files too large to highlight, say so.
- Project symbols: without a language server to answer (see Code navigation), Alt-D goes to the definition of the name at the cursor, in any loaded file. With one definition it goes straight there; with several (such as `static` functions of the same name in several files) it lists them with a preview, the current file's first. On a definition, Alt-D goes to its prototype instead, and a prototype only counts as a definition when there is none. Alt-Shift-D likewise shows the signature of the name at the cursor in the message bar, or of the function whose call the cursor is inside (the name before the innermost `(` left open on the line), preferring one in the current file, then a prototype; long signatures are shortened to fit. Alt-Shift-S lists every symbol of the loaded files by name, with `file:line` beside it, or in a window at least 80 columns wide a preview headed by `file:line`, starting on the name at the cursor. Every preview (these lists, Alt-S, project search and Ctrl-G) names its file and line on its first row. When there are several languages, only the current buffer's count, and Markdown headings are listed by Alt-Shift-S but are never a definition. These use an index of every loaded file's symbols, built the first time one of them is used: progress shows in the message bar, and Esc stops it, keeping what was indexed, so the next use carries on from there. Open buffers are indexed from their text, so a function written but not saved is found. A name that is not found first indexes again the files that changed on disk. Opening a folder with Ctrl-O drops the index.
- Code navigation: in a buffer whose language server is running (see Language servers), Alt-D asks the server where the symbol at the cursor is defined, so it also finds local variables, parameters, struct fields, macros and the file an `#include` names, and tells apart symbols of the same name in different scopes; on a definition it goes to the declaration. Files it leads to that are not loaded are added to the buffer list, as Ctrl-O adds them. When the server is not running yet or knows no definition, Alt-D uses the symbol index below. Alt-Shift-D shows the declaration of the symbol at the cursor as the server describes it (`ChangeCounts counts = ChangesCount(document->changes)`), or else the signature from the index. Alt-U lists every use of the symbol at the cursor, its declaration included, as `file:line` with the line's text, the current file's first and starting on the one at the cursor, with a preview; Enter goes there. Alt-A switches between a C header and its source file (clangd only), showing it where it was last seen. These record a jump, so Alt-[ comes back. The server answers in its own time, usually at once: an answer that comes after the cursor moved or the text changed is dropped, and one that has not come in 5 seconds is given up on (with a message). With the diagnostics panel open (Alt-K), what the server says about the symbol at the cursor (its type, its documentation and its declaration) shows below the diagnostics once the cursor has rested a moment.
- Refactoring: in a buffer whose language server is running, Alt-Shift-N renames the symbol at the cursor wherever the server knows it is used: the server first says whether it can be renamed and what its name is, which starts the `rename to:` prompt with the old name selected, so typing replaces it (Left, Right, Home or End keep it to edit), and Enter renames it. Alt-Y applies the fix the server offers for the diagnostics on the cursor's line (a missing `;`, a missing `#include`, a misspelt name: the ones whose message ends in `(fix available)`), or lists them by name when there are several, the server's preferred one selected. Ctrl-Shift-I formats the whole buffer as the server does (clangd follows the project's `.clang-format`, or else LLVM's style), indenting with spaces or tabs as Alt-T says; an edit made before the server answers leaves the buffer alone. Each changed file changes as one undo step, files that are not open are opened, and nothing is saved, as with project replace: the message counts the places and files, Alt-Shift-Z lists them and can undo the change in every file at once (see Changes across files), and Ctrl-Shift-S saves them. A rename across files finds only the uses the server knows: with clangd, those in the files of `compile_commands.json` (see Language servers).
- Git changes: in a file inside a git repository, the cell before each line number marks how the buffer differs from the file in the last commit: a green `▎` for added lines, a yellow `▎` for changed ones and a red `▔` on the line after lines that were removed (`▁` on the last line for lines removed from the end). The marks and counts follow your edits, catching up once you pause typing for 150 ms, and cover changes made by other programs before the file was opened. The status bar counts the lines added, changed and removed (`+3 ~2 -1`, leaving out a kind there are none of, so `~1` alone). Alt-Shift-] and Alt-Shift-[ go to the next and previous change, going round the end of the file. Alt-G shows a panel on the right half of the window with the change at the cursor: a few lines around it, the old lines in red and the new ones in green; it follows the cursor, takes the place of a split while it shows, and needs a window at least 80 columns wide. Alt-Z restores the change at the cursor (where its mark is) to the last commit, leaving the file's other changes alone, as one undo step. Ctrl-G compares every loaded file with the last commit (open buffers as they are in memory, the others as they are on disk, with progress in the message bar and Esc to stop), lists every change as `file:line`, as project search lists its matches, and sums up the lines they change. Without a preview each change notes its counts (`+1 ~2 -0`). Beside the list, the selected change's file is shown whole with its changes in place, starting a few lines above the selected one: the old lines on red and the new ones on green, each with a bar in the colour of the gutter marks (yellow for changed lines, green for added ones and red for removed ones), under a header with the change's counts and which of the file's changes it is. The preview scrolls on its own with Shift-Up, Shift-Down, Shift-PgUp and Shift-PgDn; Enter opens the file at the selected change. A commit made by another program is noticed when you switch to a buffer, or use Alt-G or Ctrl-G. The comparison is with the last commit (HEAD), not the staged files. The editor reads the repository itself (no `git` program is needed) and only typical ones: a work tree with a `.git` folder, with SHA-1 or SHA-256 objects, loose or packed. Worktrees and submodules (where `.git` is a file), reftable refs and bare repositories show no marks, nor do files the last commit does not have, symbolic links, or buffers over 16 MB. Line numbers must be on (Alt-L) for the marks to show.
- Language servers: a language whose package names a server (the default C package names `clangd`) starts it the first time a file of that language opens, with no command to run. The status bar shows it after the language, `C (clangd)`, with `starting` while it gets ready, `waiting` while a command (Alt-D, Alt-U, a rename, a fix, formatting) waits for its answer, and `stopped` when it could not start or ended. One server runs for each project root: the first folder above the file holding one of the package's `server-roots` (for C: `compile_commands.json`, `compile_flags.txt`, `.clangd` or `.git`), or else the folder the editor was started in. The server is told about every open file of its language and every edit, collected and sent once per keystroke or paste however many cursors made it, so it never waits for the editor and the editor never waits for it. Unnamed buffers and buffers over 16 MB are not shown to a server. Alt-V turns servers off (stopping them) and on again for the session, and when one has stopped it starts them again. A server's own messages go to `~/.local/state/editor/<language id>.log` (under `$XDG_STATE_HOME` when it is set), which is emptied when one starts, unless another server of that language is running. Opening a folder with Ctrl-O stops them, and they start again as its files open. Files are not watched for changes made by other programs, so a header changed by `git checkout` reaches the server when it is reloaded (as an open buffer is when the window regains focus), or when a file using it is saved or opened again. Quitting asks the servers to exit and waits at most half a second for them.
- Diagnostics: the errors, warnings and notes a language server reports show in the buffer as they come, a moment after you stop typing. The text they are about gets a curly underline (red for errors, yellow for warnings, blue for the rest), the number of the line they start on takes the same colour (so line numbers must be on, Alt-L), and the worst one's message follows the end of the line in faint text, cut to fit, with a count of any others starting there (`✗ use of undeclared identifier 'lenght' (+1)`). With less than 10 columns free after the line, or the end of an unwrapped line out of view, the message is left out. They move with your edits until the server reports again. The status bar counts errors and warnings (`E2 W5`). Ctrl-Shift-] and Ctrl-Shift-[ go to the next and previous one, going round the end of the file, and say what it is in the message bar. Alt-K shows a panel on the right half of the window (as Alt-G does, and in its place) with every diagnostic on the cursor's line in full: its severity, its code and source (`[-Wunused-parameter] clang`, the name to give a server to silence it), its message wrapped to fit, and its notes as `file:line: text`. Alt-Shift-K lists the diagnostics of every open buffer as `file:line: message`, with a preview. A server only reports on the files open in the editor. Which warnings show is the server's to decide: for clangd, `-Wno-...` flags or `Diagnostics: Suppress:` in `.clangd` (see Language servers).
- Statistics: Alt-I shows the lines, words, characters and bytes of the buffer (or for an image, what it is), or with a selection "selection: N of M" for each. Lines are counted as in the status bar and by `wc -l`: the empty line after a final line break is not one. A word is a run of letters, digits, `_` and non-ASCII characters, the same words that Ctrl-Left and Ctrl-Right move by, so `foo-bar` is two words (unlike `wc -w`). A character is what the cursor steps over (a grapheme cluster, so an emoji sequence is one), and a line break is one character. Bytes count line breaks as the file is saved, so two each with CRLF.
- Status bar: the buffer's name with `[+]` when it has unsaved changes and `[RO]` when its file is read only, then on the right the cursor count, git changes, errors and warnings, `wrap` while soft wrap is on, the language, `CRLF` when the file has those line endings, the line count, `buf 1/3` (open buffers of those loaded) when more than one is loaded, and `line:column` last. Only what differs from the defaults shows, and a narrow window keeps only `line:column`.
- Display: Alt-W (soft wrap, which also works in a picker and wraps the previews beside it and the Alt-G panel), Alt-L (line numbers), Alt-. (whitespace), Alt-\ (ruler), Alt-Shift-\ (ruler column, the current one selected so that typing replaces it), Alt-T (spaces or tabs for indentation). Soft wrap breaks rows a column short of the edge, so the cursor at the end of a line always fits on its last row and a line that ends exactly at the edge is not followed by an empty row. Previews have no cursor, so they wrap at the edge.
- Buffers: Ctrl-N (new unnamed buffer), Ctrl-O (open a file or a folder; Tab completes the path, and again steps through the choices; `~` stands for the home folder, here and when saving as), Ctrl-PgDn and Ctrl-PgUp (next and previous buffer in load order, among those open and the files loaded on their own, such as those named on the command line, opening them; the unopened files of an open folder are left to Ctrl-P), Ctrl-Tab (the buffer used before this one), Ctrl-P (pick any buffer; a faint `+` before a name marks unsaved changes and `·` any other open buffer, one already read), Ctrl-U (pick a buffer with unsaved changes), Ctrl-Shift-S (save every buffer with unsaved changes, asking for names for unnamed ones), Alt-Q (close the buffer, removing it from the list; with unsaved changes it asks `s` save, `d` discard or `c` cancel, as Ctrl-Q and Alt-Shift-Q do, then opens the buffer picker, and Escape there shows the most recently used open buffer, or a new unnamed one when none is open). Buffers are named by folder: a file inside an open folder shows as the folder's name followed by its path inside it (`.` opened in `~/Projects/editor` shows `editor/src/app.c`), with as many parent folders added as tell apart two open folders of the same name (`work/editor/…` and `old/editor/…`), and the deepest when folders nest. Any other file shows its path from the folder the editor was started in when it is under it (`editor src/move.c` shows `src/move.c`), or else its whole path, with `~` for the home folder. The status bar, the buffer pickers and every other list use these names, the pickers list the buffers used most recently first and the rest by name, so each folder's files come together, and the Alt-Shift-Q list names folders the same way. Ctrl-O on a folder asks `a` to add its files (those not loaded already) to the buffers, making it one more open folder, `r` to replace every buffer with them after offering to save, or `c` to cancel; when the only buffers are unnamed ones without changes it replaces them without asking. Either way the buffer picker opens: after adding, with the first new file selected and Escape leaving the current buffer, and after replacing, with Escape leaving a new empty buffer (as when the editor starts with a folder). Alt-Shift-Q closes a folder the buffers were loaded from (on the command line, or by Ctrl-O): with one open it closes that one, and with several it lists them to pick from, the folder of the current buffer selected. Every buffer of a file inside the folder closes with it (unnamed buffers and files elsewhere stay), after asking `s` save all, `d` discard or `c` cancel when any of them have unsaved changes, and Alt-Shift-R no longer walks it. Language servers left with no open file stop. When the current buffer closes with it, the picker opens as after Alt-Q. Closing a buffer shown on both sides of a split, or on the other side, shows the most recently used buffer there instead. Ctrl-Q offers to save every unsaved buffer, and does not quit if a save fails.
- Split view: Ctrl-Alt-Right splits the window into two views of the current buffer, and Ctrl-B splits it with a new unnamed buffer in the new view. Both need a window at least 80 columns wide that is not split already. The new view is on the right and takes focus. Ctrl-J moves focus to the other view, Ctrl-I swaps the two sides, and Ctrl-Alt-Left closes the focused view (its buffer stays loaded). Every other command acts on the focused view, which has the full status bar and the cursor; the other view's status bar is dimmed and shows only its name. Each view has its own cursors and scrolling, even on the same buffer, and edits in one show in the other at once. Display toggles apply to both. Below 80 columns only the focused view is shown, and the split comes back when the window widens. Opening a folder with Ctrl-O ends the split.
- Search: Ctrl-F searches as you type, highlighting every match and selecting the first one after the cursor. Up and Down (or Ctrl-P and Ctrl-N) step between matches, case matching is smart (a pattern with an upper case letter matches case, and one without matches either case; a letter after `\`, such as `\S`, does not count), Ctrl-T switches always matching case on or off (off at first, and kept for the session), Ctrl-R switches regular expressions on or off (the same way, in every search prompt), Enter keeps the match and Esc puts the cursor and view back. With a selection on one line, the prompt starts with it (escaped to match literally), as do the Ctrl-R and Ctrl-Shift-F prompts, and the first match Ctrl-F selects is the selection itself. In an empty search prompt, Up or Down brings back the last search, and while the prompt holds a search brought back, unchanged, Up goes to the one before it and Down to the one after (emptying the prompt after the newest). The last 20 searches given to any search prompt (Ctrl-W's words too) and the last 20 replacements are kept for the session, each once. Alt-N and Alt-P select the next and previous match of the last search, wrapping round the ends of the file. Ctrl-W searches for the whole word under the cursor (matching case, and always literally) and selects its next occurrence. The prompt's label shows `[match case]` (whether Ctrl-T or an upper case letter asks for it) and `[regex]` while they are on, and `bad pattern` while the typed regular expression is not valid (Enter then says why). It also shows which match is selected of how many (`[3/17]`), as do the messages after Enter, Alt-N and Alt-P (`match 3/17`), in buffers up to 4 MB with up to 10,000 matches.
- Replace: Ctrl-R asks for the text to find (Ctrl-T and Ctrl-R work in that prompt as in Ctrl-F) and its replacement, then for each match whether to replace it: `y` yes, `n` no, `a` this and all the rest, `q` (or Esc) stop. It starts at the cursor and goes round the file once, or with a selection of more than one line, replaces only the matches inside it. The whole run is one undo step.
- Project search: Ctrl-Shift-F asks for a pattern (as Ctrl-F does: smart case, Ctrl-T always matches case, and Up or Down in the empty prompt brings back earlier searches), then which files to search. Leave that blank for every loaded file, or give space-separated patterns: `*.c *.h` (by name), `src/` (a folder, at any depth), `src/*.c` (a path from the folder the editor was started in), and `!` to leave files out (`*.c !test*`). Only `!` patterns search everything else. The prompt starts with the last scope, so Enter repeats it. Files outside the scope are not read. Open buffers are searched as they are in memory, and the others are read from disk without being opened. Binary and too-large files are skipped. Progress shows in the message bar, and Esc stops the search, keeping what it found. The matching lines are listed as `file:line`, with a preview of the lines around the selected one when the window is at least 80 columns wide (narrower, each line's text follows it in the list). At most 10,000 lines are listed. Type to filter the list, and Enter opens the selected line, selecting the match and highlighting the pattern in that buffer. Alt-F shows the list again, starting after the line opened last, and Esc in the list clears it. Before showing it, Alt-F finds the matches again in every listed file that is open and has been edited, so lines that no longer match drop out and line numbers follow inserted or deleted lines; files that had no match are not searched again (Ctrl-Shift-F does that). Lines in buffers closed since the search are left out, and the list's title counts them.
- Project replace: Ctrl-R in the list of project search matches replaces the pattern on the lines the list shows, so filter it first to leave lines out. It asks for the replacement as Ctrl-R does (Up or Down in the empty prompt brings back earlier ones), then "replace in N lines of M files? (y/n)". Every match on those lines is replaced, matching each line again as it is now, so a line edited since the search is safe. A file read from disk for the search that has changed since (on disk, or opened and edited) is left alone, and the message says to search again. Files are opened but not saved: each changed buffer undoes in one step, Alt-Shift-Z lists the changed lines and can undo the replace in every file at once (see Changes across files), and Ctrl-Shift-S saves them. Esc or `n` goes back to the list; after a replace the list is cleared.
- Patterns: search text is literal unless regular expressions are on. `^` at the start ties it to the start of a line and `$` at the end to the end of one. `\t`, `\r`, `\\`, `\^` and `\$` stand for a tab, a carriage return, a backslash, `^` and `$`, in both the pattern and the replacement. `\n` is a line break in a replacement, but a match never crosses lines, so a pattern holding it never matches.
- Regular expressions (Ctrl-R in a search prompt) match within one line and find the leftmost match, preferring alternatives and repetitions in the order Perl does. A match is never empty, so `x*` finds runs of `x` and nothing between them. Ctrl-W and Ctrl-E always search literally.

  | Syntax                   | Matches                                                                                                                                   |
  | ------------------------ | ----------------------------------------------------------------------------------------------------------------------------------------- |
  | `.`                      | any character (one code point)                                                                                                            |
  | `[abc]` `[a-z]` `[^...]` | one character from the set, or not in it; `\d`, `\w` and `\s` work inside                                                                 |
  | `\d` `\w` `\s`           | a digit `0`-`9`; a word character (letter, digit, `_` or any non-ASCII character, as Ctrl-W uses); a blank (space, tab, `\r`, `\f`, `\v`) |
  | `\D` `\W` `\S`           | anything else                                                                                                                             |
  | `^` `$`                  | the start and end of the line                                                                                                             |
  | `\b` `\B`                | a word boundary, and anywhere else                                                                                                        |
  | `*` `+` `?`              | 0 or more, 1 or more, 0 or 1 (as many as possible; add `?` for as few as possible)                                                        |
  | `{m}` `{m,}` `{m,n}`     | m, at least m, m to n times (at most 1000; `?` after makes it lazy)                                                                       |
  | `(...)` `(?:...)`        | a group, captured for the replacement, or not                                                                                             |
  | `a\|b`                   | either                                                                                                                                    |
  | `\t` `\r` `\n` `\f` `\v` | control characters; `\` before any punctuation matches it literally                                                                       |

  Without case matching, letters match either case, and so do sets: `[A-Z]` also matches `a`. There are no backreferences, lookarounds, POSIX classes or Unicode properties. Search time grows with the line length times the pattern's size, never exponentially; a pattern too large (over 4096 steps, such as `(a{1000}){5}`) is refused, and so is one with groups nested more than 1000 deep. In the replacement, `\0` is the whole match and `\1` to `\9` are the groups (empty when a group took no part), and `\t`, `\r`, `\n` and `\\` work as in a literal replacement, so `(\w+)=(\w+)` replaced with `\2=\1` swaps the two sides.
- Spell check: Alt-C highlights the words that are not in the word list (`~/.config/editor/words.txt`, see Build), counts them (up to 10,000; more show as `over 10000 unknown words`) and selects the next one. Alt-N and Alt-P then step through unknown words instead of search matches, and the highlights follow your edits. Words in capitals only, words with digits or `_`, and URLs and email addresses are skipped. A capitalised word is accepted when its lowercase form is in the list. In the programming and data languages (everything highlighted except Markdown) only comments and strings are checked. In Markdown everything is checked except code (fenced and indented blocks and code spans) and link targets. Other files are checked in full. Alt-Shift-C asks whether to add the word at the cursor (or the unknown word Alt-C selected) to the list, or, when the list knows it, whether to remove it (for a capitalised word known through its lowercase form, the lowercase entry). The list is saved straight away, and read again when another program changes it.
- Syntax highlighting: Bash (`.sh`, `.bash`, and `.bashrc`, `.bash_profile`, `.profile` and `PKGBUILD` by name), C (`.c`, `.h`), CSS, Go, HTML (`.html`, `.htm`), JavaScript (`.js`, `.mjs`, `.cjs`), JSON, Markdown (`.md`, `.markdown`), PHP, SQL, XML (`.xml`, `.xsd`, `.xsl`, `.svg`), YAML (`.yaml`, `.yml`) and Zig (`.zig`, `.zon`) are coloured, and the status bar names the language. A file without an extension whose first line is `#!` running `sh` or `bash` (directly or through `env`) is Bash; this is checked when the file opens. Markdown colours headings, emphasis, links and code, and a fenced code block in any of these languages (named by the language or its extension, as in ```` ```js ````, or `shell`). HTML colours its `<script>` and `<style>` contents, and PHP the HTML around its `<?php ?>` blocks (each piece of HTML on its own, so a closing tag whose opening tag is in another piece is not coloured as a tag). Buffers over 16 MB, or that take more than a second to parse, are shown uncoloured, and the status bar says so.
- Pickers and Ctrl-H: type to filter (any part of the text, ignoring case; `^` at the start and `$` at the end tie it to the start or end, as in `^src/` or `.c$`), Up, Down, PgUp, PgDn, Home and End to move, Enter to pick, Esc to cancel. Buffers are listed most recently used first. Ctrl-H lists every binding and runs the one picked.
- Learning mode: Alt-H shows what a shortcut does in the message bar and waits for you to press it: `✓` and the next one when you are right, or `✗` and what the key you pressed does when you are not, and after three misses the answer. Only shortcuts held with Ctrl or Alt are asked, each once in a random order before any comes up again. No key runs its command while it is on; Esc stops it and says how many you got right.

## Key bindings

Ctrl-H lists every binding, filterable, and runs the one picked. The tables below are generated from the same table in `src/keymap.c` by `tools/bindings.py`, so run it after changing a binding.

<!-- bindings start: generated by tools/bindings.py -->

**Movement**

| Key        | Action                                           |
| ---------- | ------------------------------------------------ |
| Up         | move up                                          |
| Down       | move down                                        |
| Left       | move left                                        |
| Right      | move right                                       |
| Home       | move to the first non-blank, then the line start |
| End        | move to the line end                             |
| PgUp       | move up a page                                   |
| PgDn       | move down a page                                 |
| Ctrl-Left  | move to the previous word                        |
| Ctrl-Right | move to the next word                            |
| Ctrl-Home  | move to the start of the buffer                  |
| Ctrl-End   | move to the end of the buffer                    |
| Alt-Up     | scroll up a line, as the wheel does              |
| Alt-Down   | scroll down a line, as the wheel does            |

**Selection**

| Key              | Action                                                           |
| ---------------- | ---------------------------------------------------------------- |
| Shift-Up         | extend the selection up                                          |
| Shift-Down       | extend the selection down                                        |
| Shift-Left       | extend the selection left                                        |
| Shift-Right      | extend the selection right                                       |
| Shift-Home       | extend the selection to the first non-blank, then the line start |
| Shift-End        | extend the selection to the line end                             |
| Shift-PgUp       | extend the selection up a page                                   |
| Shift-PgDn       | extend the selection down a page                                 |
| Ctrl-Shift-Left  | extend the selection to the previous word                        |
| Ctrl-Shift-Right | extend the selection to the next word                            |
| Ctrl-Shift-Home  | extend the selection to the start of the buffer                  |
| Ctrl-Shift-End   | extend the selection to the end of the buffer                    |
| Ctrl-A           | select the whole buffer                                          |
| Ctrl-M           | select inside the closest brackets (again to widen)              |
| Alt-Right        | expand the selection to the enclosing syntax node (or brackets)  |
| Alt-Left         | shrink the selection back a step                                 |
| Ctrl-Shift-Up    | grow the selection by a line above                               |
| Ctrl-Shift-Down  | grow the selection by a line below                               |
| Alt-Shift-Down   | shrink the selection by a line at the top                        |
| Alt-Shift-Up     | shrink the selection by a line at the bottom                     |
| Esc              | collapse to one cursor, clear the selection, or clear highlights |

**Editing**

| Key              | Action                                                         |
| ---------------- | -------------------------------------------------------------- |
| Enter            | insert a line break                                            |
| Ctrl-Enter       | open a new line below the line                                 |
| Ctrl-Shift-Enter | open a new line above the line                                 |
| Tab              | insert a tab (or indent the selected lines)                    |
| Shift-Tab        | dedent the line (or the selected lines)                        |
| Backspace        | delete the character before the cursor                         |
| Delete           | delete the character after the cursor                          |
| Ctrl-Backspace   | delete the word before the cursor                              |
| Ctrl-Delete      | delete the word after the cursor                               |
| Ctrl-Z           | undo                                                           |
| Ctrl-Y           | redo                                                           |
| Ctrl-C           | copy the selection (or the line)                               |
| Ctrl-X           | cut the selection (or the line)                                |
| Ctrl-V           | paste                                                          |
| Ctrl-Up          | move the line (or the selected lines) up                       |
| Ctrl-Down        | move the line (or the selected lines) down                     |
| Ctrl-D           | duplicate the line (or the selected lines)                     |
| Ctrl-K           | delete the line (or the selected lines)                        |
| Ctrl-/           | toggle comments on the line (or the selected lines)            |
| Alt-J            | join the selected lines (or the line and the next)             |
| Alt-O            | sort the selected lines                                        |
| Ctrl-T           | trim trailing whitespace from the line (or the selected lines) |
| Ctrl-Shift-T     | trim trailing whitespace from every line                       |
| Alt-Shift-U      | convert the selection to upper case                            |
| Alt-Shift-L      | convert the selection to lower case                            |
| Alt-Shift-G      | align the columns of the markdown table                        |
| Alt-C            | spell check the buffer                                         |
| Alt-Shift-C      | add the word at the cursor to the word list (or remove it)     |

**Multiple cursors**

| Key           | Action                                                                      |
| ------------- | --------------------------------------------------------------------------- |
| Ctrl-E        | select the word, then add a cursor at the next occurrence (or search match) |
| Alt-E         | move the newest cursor to the next occurrence, skipping this one            |
| Ctrl-Shift-E  | add a cursor at every occurrence                                            |
| Ctrl-Alt-Up   | add a cursor on the line above                                              |
| Ctrl-Alt-Down | add a cursor on the line below                                              |
| Alt-=         | align the cursors in a column by inserting spaces                           |

**Buffers and files**

| Key          | Action                                                          |
| ------------ | --------------------------------------------------------------- |
| Ctrl-N       | create a new empty buffer                                       |
| Ctrl-O       | open a file or a folder (asks whether to add or replace)        |
| Ctrl-S       | save the buffer (asks for a name if it has none)                |
| Ctrl-Shift-S | save every buffer with unsaved changes                          |
| Alt-R        | reload the buffer from disk (asks before discarding changes)    |
| Alt-Shift-R  | load files added to the folder since it was opened              |
| Alt-Q        | close the buffer (offers to save changes)                       |
| Alt-Shift-Q  | close a folder and its buffers (offers to save changes)         |
| Alt-X        | rename, copy or delete the buffer's file                        |
| Alt-Shift-X  | copy the path of the buffer's file                              |
| Alt-;        | toggle line endings between LF and CRLF                         |
| Alt-I        | count the lines, words, characters and bytes (of the selection) |
| Ctrl-PgDn    | switch to the next buffer                                       |
| Ctrl-PgUp    | switch to the previous buffer                                   |
| Ctrl-Tab     | switch to the buffer used before this one                       |
| Ctrl-P       | pick a buffer                                                   |
| Ctrl-U       | pick a buffer with unsaved changes                              |
| Ctrl-Q       | quit (offers to save changes)                                   |

**Split view**

| Key            | Action                                                    |
| -------------- | --------------------------------------------------------- |
| Ctrl-Alt-Right | split the window, showing this buffer in both views       |
| Ctrl-B         | split the window, with a new empty buffer in the new view |
| Ctrl-Alt-Left  | close this view                                           |
| Ctrl-J         | move focus to the other view                              |
| Ctrl-I         | swap the two views                                        |

**Search**

| Key          | Action                                              |
| ------------ | --------------------------------------------------- |
| Ctrl-F       | search the buffer                                   |
| Alt-N        | go to the next match                                |
| Alt-P        | go to the previous match                            |
| Ctrl-W       | go to the next occurrence of the word at the cursor |
| Ctrl-R       | search and replace in the buffer                    |
| Ctrl-Shift-F | search the loaded files                             |
| Alt-F        | reopen the list of matches in the loaded files      |
| Ctrl-L       | go to a line number                                 |

**Navigation**

| Key         | Action                                                 |
| ----------- | ------------------------------------------------------ |
| Alt-[       | go back to where the cursor was before a jump          |
| Alt-]       | go forward again after going back                      |
| Ctrl-[      | go to the previous definition                          |
| Ctrl-]      | go to the next definition                              |
| Alt-M       | go to the matching bracket                             |
| Alt-S       | pick a symbol in this buffer                           |
| Alt-Shift-S | pick a symbol in any loaded file                       |
| Alt-D       | go to the definition of the name at the cursor         |
| Alt-Shift-D | show the signature of the name (or call) at the cursor |

**Changes since the last git commit**

| Key         | Action                                                  |
| ----------- | ------------------------------------------------------- |
| Alt-Shift-] | go to the next change                                   |
| Alt-Shift-[ | go to the previous change                               |
| Alt-G       | show or hide the change at the cursor beside the buffer |
| Alt-Z       | revert the change at the cursor to the last commit      |
| Ctrl-G      | pick a change since the last commit                     |

**Diagnostics from language servers**

| Key          | Action                                                                             |
| ------------ | ---------------------------------------------------------------------------------- |
| Ctrl-Shift-] | go to the next diagnostic                                                          |
| Ctrl-Shift-[ | go to the previous diagnostic                                                      |
| Alt-K        | show or hide the diagnostics at the cursor beside the buffer                       |
| Alt-Shift-K  | pick a diagnostic in the open buffers                                              |
| Alt-U        | pick a use of the name at the cursor (language server)                             |
| Alt-A        | switch between a header and its source (clangd)                                    |
| Alt-Shift-N  | rename the name at the cursor everywhere (language server)                         |
| Alt-Y        | fix a diagnostic on the line (language server)                                     |
| Ctrl-Shift-I | format the buffer (language server)                                                |
| Alt-Shift-Z  | list the last change across files (ctrl+z and ctrl+y in the list undo and redo it) |

**Display and settings**

| Key         | Action                                                         |
| ----------- | -------------------------------------------------------------- |
| Alt-T       | toggle indenting with spaces or tabs                           |
| Alt-W       | toggle soft wrap                                               |
| Alt-L       | toggle line numbers                                            |
| Alt-.       | toggle showing whitespace                                      |
| Alt-\       | toggle the vertical ruler                                      |
| Alt-0       | switch an image between fitting the window and its actual size |
| Alt-V       | toggle language servers (or restart stopped ones)              |
| Alt-Shift-\ | set the ruler column (and show the ruler)                      |
| Ctrl-H      | show this list of shortcuts                                    |
| Alt-H       | practise the shortcuts (esc to stop)                           |

<!-- bindings end -->

## Foot

The editor is written for foot alone. It relies on foot's kitty keyboard protocol, so that keys like Ctrl-M, Ctrl-H, Ctrl-I, Ctrl-J, Ctrl-[ and Ctrl-Shift-S reach it as themselves, and its decoder only understands the sequences foot sends. At startup it asks the terminal whether the protocol is there and exits with an error if it is not (as in the Linux console, or inside tmux).

Foot's default key bindings keep these keys for foot, so the editor never binds them: Ctrl-Shift-C and Ctrl-Shift-V (clipboard), Ctrl-Shift-R (search), Ctrl-Shift-N (new window), Ctrl-Shift-O (URLs), Ctrl-Shift-U (Unicode input), Ctrl-Shift-Z and Ctrl-Shift-X (prompt jumps), Ctrl-=, Ctrl-+, Ctrl-- and Ctrl-0 (font size), and Shift-Insert (primary paste). That is why renaming a symbol is Alt-Shift-N rather than Ctrl-Shift-R, and aligning cursors is Alt-= rather than Ctrl-=. Shift-PgUp and Shift-PgDn scroll foot's history only on the normal screen, so they reach the editor.

The window title names the current buffer, `editor: src/app.c`, with ` [+]` while it has unsaved changes. Foot keeps the title it had before on its title stack, and gets it back when the editor quits.

Images are drawn with foot's sixel graphics, sized from the cell size foot reports with the window size.

The editor turns on mouse reporting, so foot sends it clicks and the wheel instead of selecting text itself. Holding Shift still makes foot's own selection. A desktop that moves windows with Alt-drag keeps Alt-drag from reaching the editor.

Foot's Ctrl-Shift-N opens a new window in the folder of the current buffer's file, or for an unnamed buffer in the folder the editor was started with (when it was given one folder), or else where it was started. The editor tells foot the folder as it changes (OSC 7), and gives back the one it started in when it quits.

## Adding a language

A language is a folder in `~/.config/editor/languages/` (or `$XDG_CONFIG_HOME/editor/languages/`), named by its id:

```
languages/go/
  language         # what the language is (below); the only file required
  parser.so        # the tree-sitter grammar, for highlighting and symbols
  highlights.scm   # required with parser.so
  injections.scm   # optional: languages embedded in this one
  tags.scm         # optional: symbols (Alt-S, Ctrl-], Ctrl-[, Alt-D, Alt-Shift-D, Alt-Shift-S)
```

`language` holds one `key = value` per line. A line starting with `#` is a comment (there are no comments after a value, since `#` can be a comment marker):

| Key                  | Value                                                                                                                                                                                                                                                           |
| -------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `name`               | The display name, shown in the status bar. Required with `extensions`, `files` or `interpreters`, and left out for a grammar that is only embedded in others (`markdown-inline`).                                                                               |
| `extensions`         | Extensions without the dot, separated by spaces, matched ignoring case.                                                                                                                                                                                         |
| `files`              | Whole file names for files without an extension, matching case (`PKGBUILD .bashrc`).                                                                                                                                                                            |
| `interpreters`       | Programs a `#!` first line may name, directly or through `env`, for files without an extension (`sh bash`).                                                                                                                                                     |
| `comment`            | What Ctrl-/ puts before a line (`//`). Left out, the language has no comments.                                                                                                                                                                                  |
| `comment-end`        | What it puts after a line, for block comments (`-->`).                                                                                                                                                                                                          |
| `inject`             | The names other grammars embed it by, such as `bash shell`. The default is the id. Markdown fences also find a language by its extensions.                                                                                                                      |
| `symbol`             | The function of `parser.so` that gives the grammar. The default is `tree_sitter_` and the id with `-` as `_`.                                                                                                                                                   |
| `prose`              | `yes` to spell check all text outside code, as in Markdown, rather than only comments and strings.                                                                                                                                                              |
| `fresh-parse`        | `yes` to parse from scratch after every edit, for a grammar whose scanner gives different trees when parsing incrementally (Markdown's inline grammar).                                                                                                         |
| `first-pattern-wins` | `yes` when the highlight query puts its specific patterns first (Go's does): for captures of the same node, the earlier pattern wins rather than the later one.                                                                                                 |
| `server`             | The language server to start for files of the language: the program (looked up in `PATH`) and its arguments, separated by blanks, with no quoting (`clangd --background-index`). It must speak the Language Server Protocol over its standard input and output. |
| `server-roots`       | Names of files or folders that mark a project's root for the server, separated by blanks: the first folder above a file holding one of them is the root. The default is `.git`. Needs `server`.                                                                 |

Without `parser.so` a language still has its files and comment markers, just no colours or symbols. A package whose `language` file is wrong is left out, and the problem is shown when the editor starts. A `parser.so` that does not load (a missing entry point, or a tree-sitter ABI other than 13 to 15) or a query that does not compile is reported when a file of that language first opens, and that language is left uncoloured. Packages are read only at startup, so a new or changed package takes effect the next time the editor starts. Removing a language is deleting its folder.

To make a package from a grammar repository:

```sh
tools/language.sh ~/src/tree-sitter-lua lua
```

This compiles the repository's `src/parser.c` (and `src/scanner.c`, if it has one) into `parser.so`, copies its `queries/highlights.scm` and `queries/injections.scm`, and writes a `language` file to correct unless the package already has one. For a repository holding several grammars, name the folder of the one wanted (`tree-sitter-php/php`). Repositories that do not commit `src/parser.c` need `tree-sitter generate` first. Then check that the query's capture names map to theme groups (see ARCHITECTURE.md, "Colouring lines"), that any predicates it uses are among those the editor supports (an unknown one is ignored and lets its pattern match everything), and which pattern it expects to win when two capture the same node (most put general patterns first, the default). For symbols, write `tags.scm` to the contract in ARCHITECTURE.md ("Symbols"): adapt upstream's `tags.scm` if it has one, so that `@definition.<kind>` captures the whole declaration.

A `parser.so` runs inside the editor with your privileges, so only install grammars you would run as a program. Packages are only ever read from the config folder, never from a project being edited. The same goes for a `server`: it is a program the editor runs, so a project can never choose one.

### Language servers

A server only needs `server` (and, when `.git` does not mark the projects' roots, `server-roots`) in the package's `language` file; the rest comes from the server itself. How the server builds the project is its own configuration, kept with the project and shared with other tools, not the editor's. For `clangd`, that is one of:

- `compile_commands.json`, written by the build system (CMake's `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`, or `bear -- make`), which gives every file its exact flags.
- `compile_flags.txt`, one flag per line, used for every file: `-std=c23`, `-Iinclude`, `-Wall`.
- `.clangd`, YAML that adds or removes flags (`CompileFlags: Add: [-std=c23, -Iinclude]`) and says which diagnostics to show (`Diagnostics: Suppress: [unused-parameter]`), with `If: PathMatch:` blocks for parts of the tree. This repository's own `.clangd` is an example.

Only `compile_commands.json` lists every file of the project, which is what clangd's background index (`--background-index`) goes through. Without it, clangd only knows the open files and what they include, so Alt-U finds uses only there, while diagnostics, Alt-D and Alt-Shift-D work the same. `build.sh` writes this repository's `compile_commands.json` with each source's flags. The index is kept in `.cache/clangd` at the root, so leave `.cache/` out with `.editor` and `.gitignore`.

To add a language to the default packages instead, put its generated `src/parser.c` and `src/scanner.c` in `vendor/grammars/<id>/` with the grammar's `src/tree_sitter/` headers and any other files its scanner includes (when the scanner includes a header by a relative path outside `src/`, as PHP's and XML's do, keep upstream's layout), its queries in `vendor/queries/<id>/`, and the editor's own files (`language`, and `tags.scm` when it has symbols) in `languages/<id>/`. `./build.sh --languages` builds every folder in `vendor/grammars`.
