#pragma once

// a filterable list that fills the window, used to pick a buffer or a shortcut.
// typing filters the list (in a text prompt on the message bar): the filter matches any
// part of an item, ignoring case, and "^" at its start or "$" at its end ties it to the
// start or end.
// up, down, page up, page down, home and end move through the list, enter picks the
// selected item and escape cancels.
// one picker is open at a time, and while it is open it takes every key.
#include "key.h"
#include "types.h"

// draws a preview of the item with id in the area from row 0 and column, of width and
// height cells.
typedef void (*PickerPreview)(void *context, u64 id, i32 column, i32 width, i32 height);

// receives the id of the picked item, or picked is false when the picker was cancelled.
// the picker is closed before this runs, so it may open another.
typedef void (*PickerDone)(void *context, bool picked, u64 id);

// starts building a new list, dropping any previous one.
void
PickerBegin(void);

// adds an item: the text that is shown and filtered, an optional note shown after it
// (such as "[+]", or nullptr), and the id handed back when it is picked.
// text and note are copied.
void
PickerAdd(const char *text, u64 length, const char *note, u64 id);

// marks the item added last, as the buffer list marks open buffers.
// a list with a marked item draws a marker column before every item, which filtering
// leaves out.
void
PickerMark(void);

// opens the list built since PickerBegin, with the first item selected.
// title names the list in the bar above the filter, and label starts the filter prompt.
void
PickerOpen(const char *title, const char *label, PickerDone done, void *context);

// selects the item at position in the list just opened.
void
PickerSelect(u64 position);

// shows a preview of the selected item beside the list just opened, when the window is
// at least minSplitWidth columns wide.
// the notes are left out while it shows.
void
PickerShowPreview(PickerPreview preview, void *context);

// takes a key with modifiers before the filter prompt does, returning true when it used
// it.
typedef bool (*PickerKeyHandler)(void *context, Key key);

// gives the list just opened a handler for keys with modifiers.
void
PickerHandleKeys(PickerKeyHandler handler, void *context);

// how many items the filter shows, and the id of the one at position among them, in list
// order.
u64
PickerShownCount(void);

u64
PickerShownId(u64 position);

// closes the open list for a while, as when a key handler asks a question, keeping its
// items, filter and selection.
void
PickerSuspend(void);

// opens the suspended list again as it was.
void
PickerResume(void);

bool
PickerActive(void);

void
PickerKey(Key key);

// draws the list over every row above the message bar, which the prompt uses.
void
PickerDraw(i32 width, i32 height);
