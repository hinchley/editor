#include "strbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fatal.h"
#include "memory.h"

void
StrBufReserve(StrBuf *buffer, u64 capacity)
{
  if (buffer->data && capacity <= buffer->capacity) return;

  // the first allocation is exact, so text loaded once wastes nothing.
  // after that, growth is geometric so repeated appends stay linear overall.
  u64 grown = capacity > 16 ? capacity : 16;

  if (buffer->capacity) {
    grown = buffer->capacity;
    while (grown < capacity) grown *= 2;
  }

  buffer->data = MemoryResize(buffer->data, grown + 1, 1);
  buffer->capacity = grown;
  buffer->data[buffer->length] = '\0';
}

void
StrBufAppend(StrBuf *buffer, const char *data, u64 length)
{
  StrBufInsert(buffer, buffer->length, data, length);
}

void
StrBufAppendString(StrBuf *buffer, const char *string)
{
  StrBufAppend(buffer, string, strlen(string));
}

void
StrBufAppendByte(StrBuf *buffer, char byte)
{
  StrBufAppend(buffer, &byte, 1);
}

void
StrBufAppendFormat(StrBuf *buffer, const char *format, ...)
{
  va_list arguments;
  va_start(arguments, format);
  i32 needed = vsnprintf(nullptr, 0, format, arguments);
  va_end(arguments);
  if (needed < 0) Fatal("invalid format string: %s", format);

  StrBufReserve(buffer, buffer->length + (u64)needed);
  va_start(arguments, format);
  vsnprintf(buffer->data + buffer->length, (u64)needed + 1, format, arguments);
  va_end(arguments);
  buffer->length += (u64)needed;
}

void
StrBufInsert(StrBuf *buffer, u64 at, const char *data, u64 length)
{
  StrBufReserve(buffer, buffer->length + length);
  char *target = buffer->data + at;
  memmove(target + length, target, buffer->length - at);
  if (length) memcpy(target, data, length);
  buffer->length += length;
  buffer->data[buffer->length] = '\0';
}

void
StrBufRemove(StrBuf *buffer, u64 at, u64 length)
{
  if (!length) return;

  char *target = buffer->data + at;
  memmove(target, target + length, buffer->length - at - length);
  buffer->length -= length;
  buffer->data[buffer->length] = '\0';
}

void
StrBufClear(StrBuf *buffer)
{
  buffer->length = 0;
  if (buffer->data) buffer->data[0] = '\0';
}

void
StrBufFree(StrBuf *buffer)
{
  free(buffer->data);
  *buffer = (StrBuf){};
}
