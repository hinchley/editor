#pragma once

// unrecoverable errors.
// the cleanup function (set once the terminal is in raw mode) restores the terminal so
// the message is visible after exit.

void
FatalSetCleanup(void (*cleanup)(void));

[[noreturn, gnu::format(printf, 1, 2)]] void
Fatal(const char *format, ...);
