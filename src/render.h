#pragma once

// draws a frame into the screen grid: the views with their status bars, and the message
// bar.
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

  u64 openCount;
  u64 loadedCount;
} StatusInfo;

// one side of the window: a view, its syntax highlighting (or nullptr), the matches to
// highlight in it and its status.
typedef struct FramePane {
  View *view;
  Highlight *highlight;
  const MatchSet *marks;
  const StatusInfo *status;
} FramePane;

typedef struct Frame {
  i32 width;
  i32 height;

  // the panes from left to right (two when split), and which one has focus.
  FramePane panes[2];
  u32 paneCount;
  u32 focused;
  const StrBuf *message;
} Frame;

// sizes the views to the window, scrolls them to their cursors, and draws everything.
// a split narrower than minSplitWidth shows only the focused pane.
void
RenderFrame(const Frame *frame);
