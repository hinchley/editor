#include "learn.h"

#include <stdlib.h>

#include "keymap.h"
#include "memory.h"

// wrong answers to a question before its answer is shown.
static constexpr u32 missesBeforeAnswer = 3;

static StrBuf *message;
static bool active;

// the bindings asked about, in the order they are asked; all are asked before any again.
static u32 *order;
static u32 orderCount;
static u32 next;

// the binding asked about now, and the wrong answers given to it.
static const Binding *asked;
static u32 misses;

// questions answered, and those answered right before the answer was shown.
static u32 answered;
static u32 right;

void
LearnInit(StrBuf *newMessage)
{
  message = newMessage;
}

bool
LearnActive(void)
{
  return active;
}

static void
Shuffle(void)
{
  for (u32 index = orderCount - 1; index > 0; index--) {
    u32 other = arc4random_uniform(index + 1);
    u32 kept = order[index];
    order[index] = order[other];
    order[other] = kept;
  }
}

// asks about the next binding, after what message already says.
static void
Ask(void)
{
  if (next == orderCount) {
    Shuffle();
    next = 0;
  }

  asked = &keymapBindings[order[next++]];
  misses = 0;
  StrBufAppendFormat(message, "shortcut for: %s (esc to stop)", asked->help);
}

void
LearnStart(void)
{
  free(order);
  order = MemoryAllocate(keymapBindingCount * sizeof(u32));
  orderCount = 0;

  for (u32 index = 0; index < keymapBindingCount; index++) {
    const Binding *binding = &keymapBindings[index];
    bool held = binding->key.modifiers & (KeyModifierControl | KeyModifierAlt);
    if (held && binding->command != CommandLearn) order[orderCount++] = index;
  }

  Shuffle();
  next = 0;
  answered = 0;
  right = 0;
  active = true;
  Ask();
}

void
LearnKey(Key key)
{
  if (KeyEqual(key, (Key){KeyEscape, 0})) {
    active = false;
    StrBufAppendFormat(message, "learning stopped: %u of %u right", right, answered);
    return;
  }

  if (KeyEqual(key, asked->key)) {
    answered++;
    right += misses < missesBeforeAnswer;
    StrBufAppendString(message, "✓ ");
    KeyName(key, message);
    StrBufAppendString(message, " · ");
    Ask();
    return;
  }

  misses++;
  StrBufAppendString(message, "✗ ");

  if (misses >= missesBeforeAnswer) {
    StrBufAppendString(message, "the answer is ");
    KeyName(asked->key, message);
    StrBufAppendString(message, ", press it");
  } else {
    const Binding *binding = KeymapFind(key);
    KeyName(key, message);
    if (binding) StrBufAppendFormat(message, " is \"%s\", try again", binding->help);
    else StrBufAppendString(message, " is not a shortcut, try again");
  }

  StrBufAppendFormat(message, " · shortcut for: %s (esc to stop)", asked->help);
}
