#pragma once

// draws a frame into the screen grid: the view, the status bar and the message bar.
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

typedef struct Frame {
  i32 width;
  i32 height;
  View *view;

  // the matches to highlight in the view.
  const MatchSet *marks;
  const StatusInfo *status;
  const StrBuf *message;
} Frame;

// sizes the view to the window, scrolls it to its cursor, and draws everything.
void
RenderFrame(const Frame *frame);
