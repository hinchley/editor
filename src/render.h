#pragma once

// draws a frame into the screen grid: the views with their status bars, and the message
// bar.
#include "changes.h"
#include "highlight.h"
#include "match.h"
#include "strbuf.h"
#include "types.h"
#include "view.h"

// what the status bar shows about the buffer behind a view.
typedef struct StatusInfo {
  // the display path, or a name such as "[unnamed 1]".
  const StrBuf *name;
  bool modified;
  bool readOnly;

  // the language name, or nullptr when the file type is not known.
  const char *fileType;
  const char *lineEnding;

  // the lines changed since the last commit ("+3 ~2 -1"), or nullptr.
  const char *changes;

  u64 openCount;
  u64 loadedCount;
} StatusInfo;

// one side of the window: a view, its syntax highlighting (or nullptr), its changes since
// the last commit (or nullptr), the matches to highlight in it and its status.
typedef struct FramePane {
  View *view;
  Highlight *highlight;
  Changes *changes;
  const MatchSet *marks;
  const StatusInfo *status;
} FramePane;

// something drawn beside the focused pane instead of the other one, in the area from row 0
// and column, of width and height cells.
typedef struct FramePanel {
  void (*draw)(void *context, i32 column, i32 width, i32 height);
  void *context;
} FramePanel;

typedef struct Frame {
  i32 width;
  i32 height;

  // the panes from left to right (two when split), and which one has focus.
  FramePane panes[2];
  u32 paneCount;
  u32 focused;

  // a panel, when draw is set, takes the right half of a window at least minSplitWidth
  // wide, and the focused pane the left.
  FramePanel panel;
  const StrBuf *message;
} Frame;

// sizes the views to the window, scrolls them to their cursors, and draws everything.
// a split or panel narrower than minSplitWidth shows only the focused pane.
void
RenderFrame(const Frame *frame);
