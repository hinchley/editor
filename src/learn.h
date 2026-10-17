#pragma once

// learning mode: asks for the shortcuts of the key bindings one at a time, by their help
// text, in a random order, and says whether the key pressed was the one asked for.
// only bindings held with ctrl or alt are asked. while it runs it takes every key, so no
// command runs, and escape ends it.
#include "key.h"
#include "strbuf.h"

// reports the questions and answers to message.
void
LearnInit(StrBuf *message);

void
LearnStart(void);

bool
LearnActive(void);

void
LearnKey(Key key);
