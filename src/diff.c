#include "diff.h"

#include <stdlib.h>
#include <string.h>

#include "memory.h"

// the lines between the common start and end, each side as ids that are equal exactly
// when the lines are.
typedef struct Sides {
  const u32 *a;
  const u32 *b;
  u64 aStart;
  u64 bStart;

  // scratch for the forward and backward paths, sized for the whole of both sides.
  i64 *forward;
  i64 *backward;
  u64 maxCost;
  DiffHunks *out;
} Sides;

static bool
SameLine(const DiffSource *old, u64 oldIndex, const DiffSource *new, u64 newIndex)
{
  const char *a;
  const char *b;
  u64 aLength;
  u64 bLength;
  old->line(old->context, oldIndex, &a, &aLength);
  new->line(new->context, newIndex, &b, &bLength);
  return aLength == bLength && memcmp(a, b, aLength) == 0;
}

static u64
HashLine(const char *text, u64 length)
{
  // fnv-1a.
  u64 hash = 0xCBF29CE484222325ull;

  for (u64 index = 0; index < length; index++) {
    hash ^= (u8)text[index];
    hash *= 0x100000001B3ull;
  }

  return hash;
}

// a line seen while giving ids.
typedef struct Slot {
  u64 hash;
  const char *text;
  u64 length;
  u32 id;
  bool used;
} Slot;

typedef struct Interner {
  Slot *slots;
  u64 mask;
  u32 next;
} Interner;

static u32
Intern(Interner *interner, const char *text, u64 length)
{
  u64 hash = HashLine(text, length);

  for (u64 at = hash & interner->mask;; at = (at + 1) & interner->mask) {
    Slot *slot = &interner->slots[at];

    if (!slot->used) {
      *slot = (Slot){hash, text, length, interner->next++, true};
      return slot->id;
    }

    bool same = slot->hash == hash && slot->length == length;
    if (same && memcmp(slot->text, text, length) == 0) return slot->id;
  }
}

static void
AddHunk(DiffHunks *out, u64 oldStart, u64 oldCount, u64 newStart, u64 newCount)
{
  if (!oldCount && !newCount) return;

  // a hunk that starts where the last one ends joins it.
  if (out->count) {
    DiffHunk *last = &out->hunks[out->count - 1];

    bool oldJoins = last->oldStart + last->oldCount == oldStart;
    if (oldJoins && last->newStart + last->newCount == newStart) {
      last->oldCount += oldCount;
      last->newCount += newCount;
      return;
    }
  }

  if (out->count == out->capacity) {
    out->capacity = out->capacity ? out->capacity * 2 : 16;
    out->hunks = MemoryResize(out->hunks, out->capacity, sizeof(DiffHunk));
  }

  out->hunks[out->count++] = (DiffHunk){oldStart, oldCount, newStart, newCount};
}

static void Compare(Sides *sides, u64 aLow, u64 aHigh, u64 bLow, u64 bHigh);

// finds where an optimal path through a[aLow, aHigh) and b[bLow, bHigh) crosses its middle
// diagonal, searching from both ends at once, and divides there.
// returns false when the edit distance passes the limit.
static bool
Bisect(Sides *sides, u64 aLow, u64 aHigh, u64 bLow, u64 bHigh)
{
  const u32 *a = sides->a + aLow;
  const u32 *b = sides->b + bLow;
  i64 n = (i64)(aHigh - aLow);
  i64 m = (i64)(bHigh - bLow);
  i64 maxD = (n + m + 1) / 2;
  if ((u64)maxD > sides->maxCost) maxD = (i64)sides->maxCost;

  // each array is indexed by diagonal k = x - y, offset so -maxD - 1 is index 0.
  i64 offset = maxD + 1;
  i64 length = 2 * maxD + 3;
  i64 *forward = sides->forward;
  i64 *backward = sides->backward;
  for (i64 index = 0; index < length; index++) forward[index] = backward[index] = -1;
  forward[offset + 1] = 0;
  backward[offset + 1] = 0;

  i64 delta = n - m;
  bool odd = delta & 1;

  // diagonals that ran off an edge of the grid are left out from then on.
  i64 forwardStart = 0;
  i64 forwardEnd = 0;
  i64 backwardStart = 0;
  i64 backwardEnd = 0;

  for (i64 d = 0; d <= maxD; d++) {
    for (i64 k = -d + forwardStart; k <= d - forwardEnd; k += 2) {
      i64 at = offset + k;
      bool down = k == -d || (k != d && forward[at - 1] < forward[at + 1]);
      i64 x = down ? forward[at + 1] : forward[at - 1] + 1;
      i64 y = x - k;
      while (x < n && y < m && a[x] == b[y]) x++, y++;
      forward[at] = x;

      if (x > n) {
        forwardEnd += 2;
      } else if (y > m) {
        forwardStart += 2;
      } else if (odd) {
        i64 other = offset + delta - k;

        if (other >= 0 && other < length && backward[other] != -1 && x >= n - backward[other]) {
          Compare(sides, aLow, aLow + (u64)x, bLow, bLow + (u64)y);
          Compare(sides, aLow + (u64)x, aHigh, bLow + (u64)y, bHigh);
          return true;
        }
      }
    }

    for (i64 k = -d + backwardStart; k <= d - backwardEnd; k += 2) {
      i64 at = offset + k;
      bool down = k == -d || (k != d && backward[at - 1] < backward[at + 1]);
      i64 x = down ? backward[at + 1] : backward[at - 1] + 1;
      i64 y = x - k;
      while (x < n && y < m && a[n - x - 1] == b[m - y - 1]) x++, y++;
      backward[at] = x;

      if (x > n) {
        backwardEnd += 2;
      } else if (y > m) {
        backwardStart += 2;
      } else if (!odd) {
        i64 other = offset + delta - k;

        if (other >= 0 && other < length && forward[other] != -1) {
          i64 forwardX = forward[other];
          i64 forwardY = offset + forwardX - other;

          if (forwardX >= n - x) {
            Compare(sides, aLow, aLow + (u64)forwardX, bLow, bLow + (u64)forwardY);
            Compare(sides, aLow + (u64)forwardX, aHigh, bLow + (u64)forwardY, bHigh);
            return true;
          }
        }
      }
    }
  }

  return false;
}

static void
Compare(Sides *sides, u64 aLow, u64 aHigh, u64 bLow, u64 bHigh)
{
  while (aLow < aHigh && bLow < bHigh && sides->a[aLow] == sides->b[bLow]) aLow++, bLow++;
  while (aLow < aHigh && bLow < bHigh && sides->a[aHigh - 1] == sides->b[bHigh - 1]) {
    aHigh--;
    bHigh--;
  }

  u64 oldStart = sides->aStart + aLow;
  u64 newStart = sides->bStart + bLow;

  if (aLow == aHigh || bLow == bHigh || !Bisect(sides, aLow, aHigh, bLow, bHigh)) {
    AddHunk(sides->out, oldStart, aHigh - aLow, newStart, bHigh - bLow);
  }
}

void
DiffRun(const DiffSource *old, const DiffSource *new, u64 maxCost, DiffHunks *out)
{
  out->count = 0;
  u64 start = 0;
  u64 oldEnd = old->count;
  u64 newEnd = new->count;

  while (start < oldEnd && start < newEnd && SameLine(old, start, new, start)) start++;
  while (oldEnd > start && newEnd > start && SameLine(old, oldEnd - 1, new, newEnd - 1)) {
    oldEnd--;
    newEnd--;
  }

  u64 n = oldEnd - start;
  u64 m = newEnd - start;

  if (!n || !m) {
    AddHunk(out, start, n, start, m);
    return;
  }

  // ids for the lines in between, from a table at most half full.
  u64 size = 16;
  while (size < 2 * (n + m)) size *= 2;
  Interner interner = {MemoryAllocateZeroed(size, sizeof(Slot)), size - 1, 0};
  u32 *a = MemoryAllocate(n * sizeof(u32));
  u32 *b = MemoryAllocate(m * sizeof(u32));
  const char *text;
  u64 length;

  for (u64 index = 0; index < n; index++) {
    old->line(old->context, start + index, &text, &length);
    a[index] = Intern(&interner, text, length);
  }

  for (u64 index = 0; index < m; index++) {
    new->line(new->context, start + index, &text, &length);
    b[index] = Intern(&interner, text, length);
  }

  free(interner.slots);

  u64 maxD = (n + m + 1) / 2;
  if (maxD > maxCost) maxD = maxCost;
  Sides sides = {
    .a = a,
    .b = b,
    .aStart = start,
    .bStart = start,
    .forward = MemoryAllocate((2 * maxD + 3) * sizeof(i64)),
    .backward = MemoryAllocate((2 * maxD + 3) * sizeof(i64)),
    .maxCost = maxCost,
    .out = out
  };

  Compare(&sides, 0, n, 0, m);

  free(sides.forward);
  free(sides.backward);
  free(a);
  free(b);
}

void
DiffFree(DiffHunks *hunks)
{
  free(hunks->hunks);
  *hunks = (DiffHunks){};
}
