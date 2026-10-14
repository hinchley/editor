# editor

A personal terminal text editor for Linux, written in C23.

It targets Wayland and the foot, alacritty and ghostty terminals, and requires the Kitty Keyboard Protocol. There is no fallback for legacy terminals.

> Status: early development (milestone M1, terminal and input). Running `editor` shows a temporary key check screen instead of an editor: it names each key as it arrives, lists the bindings not yet pressed, and logs pastes. Ctrl-Q quits, and Ctrl-Alt-Q forces a fatal error to check that the terminal is restored.

## Build

```sh
./build.sh           # optimised build, produces ./editor
./build.sh --debug   # unoptimised build with address and undefined behaviour sanitizers
```

Requirements: gcc 15 or newer (for C23 `#embed`), bash, and `wl-clipboard` at runtime for system clipboard support.

The build compiles the vendored tree-sitter sources once into `build/vendor` and only recompiles them when they change. The editor sources are rebuilt every time. The build fails if `data/words.txt` is not sorted in byte order.

## Usage

```sh
editor                       # new empty buffer
editor file                  # open file (created on first save if it does not exist)
editor file1 path/to/file2   # load both and pick one
editor folder                # load every file under folder and pick one
```

Key bindings will be documented here as features land. They follow the spec, with one change: project search is Alt-Shift-F rather than Ctrl-Shift-F, because alacritty and ghostty use Ctrl-Shift-F for their own search.

### Ghostty

Ghostty's default key bindings take Shift-Home, Shift-End, Shift-PgUp, Shift-PgDn (scrolling) and Ctrl-Tab (next tab), so the editor never sees them. Foot and alacritty pass them through. To use the editor in ghostty, add this to `~/.config/ghostty/config.ghostty`:

```
keybind = shift+home=unbind
keybind = shift+end=unbind
keybind = shift+page_up=unbind
keybind = shift+page_down=unbind
keybind = ctrl+tab=unbind
```

If changing the ghostty config is not an option, rebind them in `src/keymap.c` instead. Suggested free keys: Alt-Left, Alt-Right, Alt-Up and Alt-Down for the four selection keys, and Ctrl-Space for Ctrl-Tab.
