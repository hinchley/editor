#pragma once

// questions asked in the message bar.
// a text prompt edits one line of input until enter accepts it or escape cancels it.
// a choice prompt waits for one of a few letter keys, or escape.
// one prompt is open at a time, and while it is open it takes every key.
#include "key.h"
#include "strbuf.h"
#include "types.h"

// receives the answer: the text for a text prompt, the chosen letter for a choice, or
// nullptr when the prompt was cancelled.
// the prompt is closed before this runs, so it may open another.
typedef void (*PromptDone)(void *context, const char *answer, u64 length);

// told about the text of a text prompt each time it changes.
typedef void (*PromptChanged)(void *context, const char *text, u64 length);

// asked to complete the text of a text prompt when tab is pressed.
// the cursor goes to the end of the text afterwards.
typedef void (*PromptComplete)(void *context, StrBuf *text);

// opens a text prompt showing label, with initial as the starting text.
void
PromptAsk(const char *label, const char *initial, PromptDone done, void *context);

// sets the change and tab handlers of the text prompt just opened (either may be
// nullptr).
// they are dropped when the prompt closes.
void
PromptHandle(PromptChanged changed, PromptComplete complete);

// changes the label of the open prompt.
void
PromptSetLabel(const char *label);

// replaces the text of the open text prompt, with the cursor at its end, and reports the
// change.
void
PromptSetText(const char *text, u64 length);

// the text typed into the open text prompt so far.
const StrBuf *
PromptText(void);

// opens a choice prompt showing question; choices holds the accepted lowercase letters.
void
PromptChoose(const char *question, const char *choices, PromptDone done, void *context);

bool
PromptActive(void);

void
PromptKey(Key key);

// adds pasted text to a text prompt, up to its first line break.
void
PromptPaste(const char *text, u64 length);

// draws the open prompt on a screen row and puts the terminal cursor in it.
void
PromptDraw(i32 row, i32 width);
