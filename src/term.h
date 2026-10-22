#pragma once

// the terminal session in foot: raw mode, the alternate screen, the kitty keyboard
// protocol, bracketed paste, mouse reporting, the cursor shape, window size and resize
// notification.
// every way out of the program goes through TermStop, so the terminal is always restored.
#include <poll.h>

#include "types.h"

// the window in cells, and its text area in pixels (0 when the terminal does not say).
typedef struct TermSize {
  i32 width;
  i32 height;
  i32 pixelWidth;
  i32 pixelHeight;
} TermSize;

enum TermWait {
  TermWaitInput,
  TermWaitResize,
  TermWaitTimeout,

  // one of the other descriptors is ready.
  TermWaitOther
};

// puts the terminal into editor mode.
// ends the program with a message if the terminal does not answer as foot does (it has no
// kitty keyboard protocol, as in the linux console or tmux), leaving it as it was.
void
TermStart(void);

// restores the terminal.
// safe to call more than once and from a signal handler.
void
TermStop(void);

// window size from the kernel, which foot keeps in pixels too.
TermSize
TermGetSize(void);

// waits for input, a resize, or one of count other descriptors (at most 16) to be ready.
// a negative timeout waits forever.
enum TermWait
TermWait(i32 timeoutMilliseconds, const struct pollfd *others, u32 count);

// reads whatever input is available and passes it to the input decoder.
void
TermReadInput(void);

// writes all bytes to the terminal.
void
TermWrite(const char *data, u64 length);
