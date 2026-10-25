#pragma once

// places: a picker of places in the loaded files, each an item (usually "file:line") with
// a preview of the lines around it, and going to one, recording a jump.
// the language server commands (definitions, references) and the list of diagnostics
// use it.
#include "buffer.h"
#include "types.h"

// starts a new list, dropping the last one.
void
PlacesBegin(void);

// adds a place, range in the entry with serial, shown as text with a note (or nullptr)
// and marked with a symbol one column wide (or nullptr).
// the preview picks out the range when it is on one line.
void
PlacesAdd(
  u64 serial,
  Range range,
  const char *text,
  u64 length,
  const char *note,
  const char *mark
);

// opens the picker of the places added, with title, starting on the place added at
// selected.
void
PlacesOpen(const char *title, u64 selected);
