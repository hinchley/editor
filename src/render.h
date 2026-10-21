#pragma once

// draws a frame into the screen grid: the views with their status bars, and the message
// bar.
#include "changes.h"
#include "diagnostics.h"
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

  // the errors and warnings its language server reports ("E2 W1"), or nullptr.
  const char *diagnostics;

  u64 openCount;
  u64 loadedCount;
} StatusInfo;

// one side of the window: a view, its syntax highlighting (or nullptr), its changes since
// the last commit (or nullptr), its language server's diagnostics (or nullptr), the
// matches to highlight in it and its status.
typedef struct FramePane {
  View *view;
  Highlight *highlight;
  Changes *changes;
  const Diagnostics *diagnostics;
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

static constexpr u32 renderNoPane = UINT32_MAX;

// the index, among the last frame's panes, of the one whose text or gutter holds a screen
// cell, or renderNoPane for a status bar, the separator, a panel or the message bar.
u32
RenderPaneAt(i32 row, i32 column);

// the index, among the last frame's panes, of the one whose status bar holds a screen cell,
// or renderNoPane.
u32
RenderStatusAt(i32 row, i32 column);
