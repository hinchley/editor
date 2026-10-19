#include "diagnostics.h"

#include <stdlib.h>

#include "memory.h"

const char *
DiagnosticSeverityName(enum DiagnosticSeverity severity)
{
  switch (severity) {
  case DiagnosticSeverityError: return "error";
  case DiagnosticSeverityWarning: return "warning";
  case DiagnosticSeverityInformation: return "information";
  case DiagnosticSeverityHint: return "hint";
  }

  return "error";
}

const char *
DiagnosticSeveritySymbol(enum DiagnosticSeverity severity)
{
  if (severity == DiagnosticSeverityError) return "\u2717";
  return severity == DiagnosticSeverityWarning ? "\u25B2" : "\u25CF";
}

enum ThemeGroup
DiagnosticSeverityGroup(enum DiagnosticSeverity severity)
{
  if (severity == DiagnosticSeverityError) return ThemeError;
  return severity == DiagnosticSeverityWarning ? ThemeWarning : ThemeInfo;
}

void
DiagnosticsClear(Diagnostics *diagnostics)
{
  diagnostics->count = 0;
  diagnostics->span = 0;
  StrBufClear(&diagnostics->text);
}

void
DiagnosticsFree(Diagnostics *diagnostics)
{
  free(diagnostics->items);
  StrBufFree(&diagnostics->text);
  *diagnostics = (Diagnostics){};
}

u32
DiagnosticsAddText(Diagnostics *diagnostics, const char *text, u64 length)
{
  u32 offset = (u32)diagnostics->text.length;
  StrBufAppend(&diagnostics->text, text, length);
  StrBufAppendByte(&diagnostics->text, '\0');
  return offset;
}

void
DiagnosticsAdd(Diagnostics *diagnostics, const Diagnostic *diagnostic)
{
  if (diagnostics->count == diagnostics->capacity) {
    diagnostics->capacity = diagnostics->capacity ? diagnostics->capacity * 2 : 32;
    diagnostics->items = MemoryResize(diagnostics->items, diagnostics->capacity,
      sizeof(Diagnostic));
  }

  diagnostics->items[diagnostics->count++] = *diagnostic;
  u64 span = diagnostic->range.end.line - diagnostic->range.start.line;
  if (span > diagnostics->span) diagnostics->span = span;
}

static int
CompareDiagnostics(const void *first, const void *second)
{
  const Diagnostic *a = first;
  const Diagnostic *b = second;
  i32 order = PositionCompare(a->range.start, b->range.start);
  if (order) return order;
  return (int)a->severity - (int)b->severity;
}

void
DiagnosticsSort(Diagnostics *diagnostics)
{
  if (diagnostics->count < 2) return;

  qsort(diagnostics->items, diagnostics->count, sizeof(Diagnostic), CompareDiagnostics);
}

void
DiagnosticsAdjust(Diagnostics *diagnostics, const BufferChange *change)
{
  // adjusting keeps positions in order, so the set stays sorted, but a line break typed
  // inside a diagnostic makes it span more lines.
  diagnostics->span = 0;

  for (u32 index = 0; index < diagnostics->count; index++) {
    Range *range = &diagnostics->items[index].range;
    range->start = BufferAdjust(range->start, change);
    range->end = BufferAdjust(range->end, change);
    u64 span = range->end.line - range->start.line;
    if (span > diagnostics->span) diagnostics->span = span;
  }
}

// the first diagnostic starting on or after line.
static u32
FirstFrom(const Diagnostics *diagnostics, u64 line)
{
  u32 low = 0;
  u32 high = diagnostics->count;

  while (low < high) {
    u32 middle = low + (high - low) / 2;

    if (diagnostics->items[middle].range.start.line < line) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }

  return low;
}

void
DiagnosticsAround(const Diagnostics *diagnostics, u64 line, u32 *first, u32 *end)
{
  *first = FirstFrom(diagnostics, line > diagnostics->span ? line - diagnostics->span : 0);
  *end = FirstFrom(diagnostics, line + 1);
}

bool
DiagnosticsOnLine(const Diagnostics *diagnostics, u64 line, u32 *worst, u32 *count)
{
  u32 first = FirstFrom(diagnostics, line);
  u32 end = FirstFrom(diagnostics, line + 1);
  if (first == end) return false;

  *worst = first;
  *count = end - first;

  for (u32 index = first + 1; index < end; index++) {
    if (diagnostics->items[index].severity < diagnostics->items[*worst].severity) {
      *worst = index;
    }
  }

  return true;
}

bool
DiagnosticsStep(const Diagnostics *diagnostics, Position position, bool backward, u32 *index,
  bool *wrapped)
{
  u32 count = diagnostics->count;
  if (!count) return false;

  *wrapped = false;

  if (backward) {
    for (u32 at = count; at-- > 0;) {
      if (PositionCompare(diagnostics->items[at].range.start, position) < 0) {
        *index = at;
        return true;
      }
    }

    *wrapped = true;
    *index = count - 1;
    return true;
  }

  for (u32 at = 0; at < count; at++) {
    if (PositionCompare(diagnostics->items[at].range.start, position) > 0) {
      *index = at;
      return true;
    }
  }

  *wrapped = true;
  *index = 0;
  return true;
}

DiagnosticCounts
DiagnosticsCount(const Diagnostics *diagnostics)
{
  DiagnosticCounts counts = {};

  for (u32 index = 0; index < diagnostics->count; index++) {
    enum DiagnosticSeverity severity = diagnostics->items[index].severity;
    if (severity == DiagnosticSeverityError) counts.errors++;
    else if (severity == DiagnosticSeverityWarning) counts.warnings++;
  }

  return counts;
}
