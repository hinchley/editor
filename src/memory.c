#include "memory.h"

#include <stdckdint.h>
#include <stdlib.h>
#include <string.h>

// glibc alone has malloc_trim, and __GLIBC__ comes from the headers above.
#ifdef __GLIBC__
#include <malloc.h>
#endif

#include "fatal.h"

void *
MemoryAllocate(u64 size)
{
  // malloc(0) may return null, so always ask for at least one byte.
  void *pointer = malloc(size ? size : 1);
  if (!pointer) Fatal("out of memory allocating %lu bytes", size);
  return pointer;
}

void *
MemoryAllocateZeroed(u64 count, u64 size)
{
  void *pointer = calloc(count ? count : 1, size ? size : 1);
  if (!pointer) Fatal("out of memory allocating %lu x %lu bytes", count, size);
  return pointer;
}

void *
MemoryResize(void *pointer, u64 count, u64 size)
{
  u64 total;
  if (ckd_mul(&total, count, size)) Fatal("allocation size overflow (%lu x %lu)", count, size);

  void *resized = realloc(pointer, total ? total : 1);
  if (!resized) Fatal("out of memory resizing to %lu bytes", total);
  return resized;
}

void *
MemoryDuplicate(const void *source, u64 size)
{
  void *pointer = MemoryAllocate(size);
  if (size) memcpy(pointer, source, size);
  return pointer;
}

void
MemoryTrim(void)
{
#ifdef __GLIBC__
  malloc_trim(0);
#endif
}
