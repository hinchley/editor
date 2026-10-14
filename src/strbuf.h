#pragma once

// growable byte string.
// a zeroed StrBuf is a valid empty string.
// once memory is allocated, data is always nul terminated so it can be passed to c apis.
#include "types.h"

typedef struct StrBuf {
  char *data;
  u64 length;
  u64 capacity;
} StrBuf;

// ensures room for at least capacity bytes plus the terminator.
void
StrBufReserve(StrBuf *buffer, u64 capacity);

void
StrBufAppend(StrBuf *buffer, const char *data, u64 length);

void
StrBufAppendString(StrBuf *buffer, const char *string);

void
StrBufAppendByte(StrBuf *buffer, char byte);

[[gnu::format(printf, 2, 3)]] void
StrBufAppendFormat(StrBuf *buffer, const char *format, ...);

void
StrBufInsert(StrBuf *buffer, u64 at, const char *data, u64 length);

void
StrBufRemove(StrBuf *buffer, u64 at, u64 length);

// empties the string but keeps its memory.
void
StrBufClear(StrBuf *buffer);

// releases the memory and leaves an empty string.
void
StrBufFree(StrBuf *buffer);
