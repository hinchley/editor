#pragma once

// checked allocation.
// every function either succeeds or ends the program through Fatal.
// release memory with free.
#include "types.h"

void *
MemoryAllocate(u64 size);

// zero filled allocation of count elements.
void *
MemoryAllocateZeroed(u64 count, u64 size);

// resizes to hold count elements of size bytes.
void *
MemoryResize(void *pointer, u64 count, u64 size);

void *
MemoryDuplicate(const void *source, u64 size);

// hands memory freed to the heap back to the system, where libc keeps it for the process
// (glibc does; musl returns it as it is freed).
void
MemoryTrim(void);
