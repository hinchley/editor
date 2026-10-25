#pragma once

// diagnostics: the errors, warnings and notes a language server reports for a document,
// kept in order of where they start.
// a server replaces them all at once, after it has looked at the text again, and in
// between they follow the buffer's changes, so each stays on its text while you type.
#include "buffer.h"
#include "strbuf.h"
#include "theme.h"
#include "types.h"

// the severities of the language server protocol, worst first.
enum DiagnosticSeverity : u8 {
  DiagnosticSeverityError = 1,
  DiagnosticSeverityWarning = 2,
  DiagnosticSeverityInformation = 3,
  DiagnosticSeverityHint = 4
};

typedef struct Diagnostic {
  Range range;
  enum DiagnosticSeverity severity;

  // where its texts start in the set's text, each nul terminated (and empty when the
  // server gave none): the message, the code ("-Wunused-parameter") and the source
  // ("clang"), and the related notes, one "file:line: text" a line.
  u32 message;
  u32 code;
  u32 source;
  u32 notes;
} Diagnostic;

typedef struct Diagnostics {
  Diagnostic *items;
  u32 count;
  u32 capacity;
  StrBuf text;

  // the most lines any one diagnostic spans after its first, which bounds how far back a
  // diagnostic reaching a line can start.
  u64 span;
} Diagnostics;

// what the status bar counts.
typedef struct DiagnosticCounts {
  u32 errors;
  u32 warnings;
} DiagnosticCounts;

// how a severity is named ("warning"), the symbol one column wide that marks it, and the
// theme group it draws in (information and hints share one).
const char *
DiagnosticSeverityName(enum DiagnosticSeverity severity);

const char *
DiagnosticSeveritySymbol(enum DiagnosticSeverity severity);

enum ThemeGroup
DiagnosticSeverityGroup(enum DiagnosticSeverity severity);

void
DiagnosticsClear(Diagnostics *diagnostics);

void
DiagnosticsFree(Diagnostics *diagnostics);

// adds a text to the set's text, nul terminated, returning where it starts for the
// fields of a Diagnostic.
u32
DiagnosticsAddText(Diagnostics *diagnostics, const char *text, u64 length);

// adds a diagnostic; DiagnosticsSort must follow the last one.
void
DiagnosticsAdd(Diagnostics *diagnostics, const Diagnostic *diagnostic);

// orders the diagnostics by where they start, and then by severity.
void
DiagnosticsSort(Diagnostics *diagnostics);

// moves every diagnostic as the change moved the text it was on.
void
DiagnosticsAdjust(Diagnostics *diagnostics, const BufferChange *change);

static inline const char *
DiagnosticsText(const Diagnostics *diagnostics, u32 offset)
{
  return diagnostics->text.data + offset;
}

// the diagnostics that may reach line: from first up to (not including) end, every
// diagnostic that starts no more than span lines before it, up to the last starting on it.
void
DiagnosticsAround(const Diagnostics *diagnostics, u64 line, u32 *first, u32 *end);

// the worst diagnostic starting on line, and how many start there; false when none does.
bool
DiagnosticsOnLine(const Diagnostics *diagnostics, u64 line, u32 *worst, u32 *count);

// the first diagnostic starting after position or (backward) the last starting before
// it, going round the end of the buffer; wrapped says whether it went round.
// false when there are none.
bool
DiagnosticsStep(
  const Diagnostics *diagnostics,
  Position position,
  bool backward,
  u32 *index,
  bool *wrapped
);

DiagnosticCounts
DiagnosticsCount(const Diagnostics *diagnostics);
