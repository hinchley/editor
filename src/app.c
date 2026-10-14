#include "app.h"

#include <stdlib.h>

#include "input.h"
#include "keycheck.h"
#include "screen.h"
#include "term.h"

// how long an incomplete escape sequence may wait for the rest of its bytes.
static constexpr i32 escapeTimeout = 50;

static TermSize windowSize;

static void
Resize(void)
{
  windowSize = TermGetSize();
  ScreenResize(windowSize.width, windowSize.height);
}

static void
WaitForInput(void)
{
  switch (TermWait(InputPending() ? escapeTimeout : -1)) {
  case TermWaitInput:
    TermReadInput();
    break;
  case TermWaitResize:
    Resize();
    KeyCheckResized();
    break;
  case TermWaitTimeout:
    InputExpire();
    break;
  }
}

i32
AppRun([[maybe_unused]] i32 argumentCount, [[maybe_unused]] char **arguments)
{
  // command line arguments are handled from M2 on.
  TermStart();
  Resize();

  bool running = true;

  while (running) {
    // handle everything already buffered, then draw once.
    InputEvent event;
    while (running && InputNext(&event)) running = KeyCheckHandle(&event);

    KeyCheckDraw(windowSize.width, windowSize.height);
    ScreenFlush();
    if (running) WaitForInput();
  }

  TermStop();
  return EXIT_SUCCESS;
}
