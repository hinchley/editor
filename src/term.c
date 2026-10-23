#include "term.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "fatal.h"
#include "input.h"
#include "settings.h"
#include "strbuf.h"

// cursor shapes understood by the DECSCUSR sequence.
enum CursorShape {
  CursorShapeBlinkingBlock = 1,
  CursorShapeSteadyBlock = 2,
  CursorShapeBlinkingBar = 5,
  CursorShapeSteadyBar = 6
};

// kitty keyboard protocol enhancement flags.
// disambiguation is enough: it gives every key with ctrl or alt its own sequence and
// sends escape as a sequence, while plain text still arrives as utf-8.
enum KeyboardFlag {
  KeyboardFlagDisambiguate = 1
};

// how long the terminal has to answer the startup queries.
static constexpr i32 handshakeTimeout = 1000;

// leaves editor mode in the reverse order of entering it.
// the keyboard protocol keeps a separate stack per screen, so it is popped before
// leaving the alternate screen.
// clang-format off
static constexpr char leaveSequence[] =
  "\x1b[?1006l"  // stop sgr mouse coordinates
  "\x1b[?1002l"  // stop mouse reporting
  "\x1b[<u"      // pop the keyboard protocol flags
  "\x1b[?2004l"  // disable bracketed paste
  "\x1b[0 q"     // default cursor shape
  "\x1b[0m"      // default colours
  "\x1b[?25h"    // show the cursor
  "\x1b[?1049l"  // leave the alternate screen
  "\x1b[23;0t";  // give back the window title
// clang-format on

static struct termios savedMode;
static i32 wakePipe[2] = {-1, -1};

// what needs undoing, read by signal handlers.
static volatile sig_atomic_t rawModeActive;
static volatile sig_atomic_t screenActive;

// the window title last set.
static StrBuf title;

// the folder last given to foot for its new windows, the sequence that gives back the one
// the editor started in, written on leaving, and the host's name, which the folder's
// address starts with.
static StrBuf folder;
static StrBuf leaveFolder;
static char host[256];

// set when the terminal has gone (a hang up or termination signal, the end of input, or a
// failed write), so the main loop keeps copies of the work and exits.
static volatile sig_atomic_t hungUp;

// writes everything without allocating or exiting, so it is safe in a signal handler.
static bool
WriteAll(const char *data, u64 length)
{
  while (length) {
    i64 written = write(STDOUT_FILENO, data, length);

    if (written < 0) {
      if (errno == EINTR) continue;
      return false;
    }

    data += written;
    length -= (u64)written;
  }

  return true;
}

static void
ResizeHandler([[maybe_unused]] int signal)
{
  // wakes the main loop.
  // a full pipe already holds a wake up, so a failed write is fine.
  i32 savedError = errno;
  char byte = 0;
  i64 ignored = write(wakePipe[1], &byte, 1);
  (void)ignored;
  errno = savedError;
}

static void
HangUpHandler(int signal)
{
  hungUp = 1;
  ResizeHandler(signal);
}

static void
StopHandler(int signal)
{
  // the handler was installed with SA_RESETHAND, so the raised signal takes its default
  // action (terminate or dump core) once this handler returns.
  TermStop();
  raise(signal);
}

static void
InstallSignals(void)
{
  if (pipe(wakePipe) != 0) Fatal("cannot create a pipe: %s", strerror(errno));

  for (u32 index = 0; index < 2; index++) {
    fcntl(wakePipe[index], F_SETFL, O_NONBLOCK);
    fcntl(wakePipe[index], F_SETFD, FD_CLOEXEC);
  }

  struct sigaction action = {};
  sigemptyset(&action.sa_mask);
  action.sa_handler = ResizeHandler;
  action.sa_flags = SA_RESTART;
  sigaction(SIGWINCH, &action, nullptr);

  // a clipboard command that exits early must not end the editor through its pipe.
  signal(SIGPIPE, SIG_IGN);

  // nothing is written from these: after a crash the heap and buffers may be damaged, and
  // recover.c's last idle copy is already on disk.
  static constexpr i32 stopSignals[] =
    {SIGINT, SIGQUIT, SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT};
  action.sa_handler = StopHandler;
  action.sa_flags = (i32)SA_RESETHAND;

  for (u32 index = 0; index < sizeof(stopSignals) / sizeof(stopSignals[0]); index++) {
    sigaction(stopSignals[index], &action, nullptr);
  }

  action.sa_handler = HangUpHandler;
  action.sa_flags = SA_RESTART;
  sigaction(SIGHUP, &action, nullptr);
  sigaction(SIGTERM, &action, nullptr);
}

i64
TermNow(void)
{
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return now.tv_sec * 1000 + now.tv_nsec / 1'000'000;
}

static void
Handshake(void)
{
  // ask for the keyboard protocol flags, then the cursor position.
  // every terminal answers the second, so a cursor report without a keyboard report
  // before it means the protocol is not supported, and this is not foot.
  static constexpr char query[] = "\x1b[?u\x1b[6n";
  TermWrite(query, sizeof(query) - 1);

  bool keyboardProtocol = false;
  i64 deadline = TermNow() + handshakeTimeout;

  while (true) {
    InputEvent event;

    while (InputNext(&event)) {
      if (event.type == InputEventKeyboardReport) keyboardProtocol = true;
      if (event.type != InputEventCursorReport) continue;
      if (!keyboardProtocol) Fatal("no kitty keyboard protocol (the editor runs in foot)");

      return;
    }

    if (hungUp) Fatal("the terminal was closed");
    i64 remaining = deadline - TermNow();
    if (remaining <= 0) Fatal("the terminal did not answer the cursor position request");
    if (TermWait((i32)remaining, nullptr, 0) == TermWaitInput) TermReadInput();
  }
}

// appends the sequence (OSC 7) that tells foot new windows open in path, an absolute
// folder, as a file address with every byte but unreserved ones and "/" percent-encoded.
static void
AppendFolder(StrBuf *sequence, const char *path, u64 length)
{
  StrBufAppendFormat(sequence, "\x1b]7;file://%s", host);

  for (u64 index = 0; index < length; index++) {
    u8 byte = (u8)path[index];
    bool plain = isalnum(byte) || strchr("-._~/", byte);
    if (plain && byte) StrBufAppendByte(sequence, (char)byte);
    else StrBufAppendFormat(sequence, "%%%02X", byte);
  }

  StrBufAppendString(sequence, "\x1b\\");
}

static void
EnterEditorMode(void)
{
  enum CursorShape shape = cursorBlink ? CursorShapeBlinkingBar : CursorShapeSteadyBar;

  if (cursorStyle == CursorStyleBlock) {
    shape = cursorBlink ? CursorShapeBlinkingBlock : CursorShapeSteadyBlock;
  }

  // the window title is kept on the terminal's stack, to be given back on leaving.
  // mouse reports cover presses, releases and motion while a button is held (1002), in the
  // sgr form, which has no limit on the coordinates (1006).
  char sequence[64];
  i32 length = snprintf(
    sequence,
    sizeof(sequence),
    "\x1b[22;0t\x1b[?1049h\x1b[>%du\x1b[?2004h\x1b[%d q\x1b[?1002h\x1b[?1006h",
    KeyboardFlagDisambiguate,
    shape
  );

  screenActive = 1;
  TermWrite(sequence, (u64)length);
}

void
TermStart(void)
{
  if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
    Fatal("standard input and output must be a terminal");
  }

  if (tcgetattr(STDIN_FILENO, &savedMode) != 0) {
    Fatal("cannot read the terminal mode: %s", strerror(errno));
  }

  InstallSignals();
  FatalSetCleanup(TermStop);

  struct termios raw = savedMode;
  cfmakeraw(&raw);
  rawModeActive = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
    Fatal("cannot set raw mode: %s", strerror(errno));
  }

  Handshake();

  // foot's new windows go back to opening where the editor started.
  if (gethostname(host, sizeof(host) - 1) != 0) host[0] = 0;
  char started[PATH_MAX];
  if (getcwd(started, sizeof(started))) AppendFolder(&leaveFolder, started, strlen(started));

  EnterEditorMode();
}

void
TermStop(void)
{
  if (screenActive) {
    screenActive = 0;
    WriteAll(leaveSequence, sizeof(leaveSequence) - 1);
    if (leaveFolder.length) WriteAll(leaveFolder.data, leaveFolder.length);
  }

  // discard unread input so leftover key sequences do not reach the shell.
  if (rawModeActive) {
    rawModeActive = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &savedMode);
  }
}

TermSize
TermGetSize(void)
{
  struct winsize size;

  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) != 0) {
    Fatal("cannot read the window size: %s", strerror(errno));
  }

  return (TermSize){size.ws_col, size.ws_row, size.ws_xpixel, size.ws_ypixel};
}

enum TermWait
TermWait(i32 timeoutMilliseconds, const struct pollfd *others, u32 count)
{
  struct pollfd sources[2 + 16] = {
    {.fd = STDIN_FILENO, .events = POLLIN},
    {.fd = wakePipe[0], .events = POLLIN}
  };

  if (count > 16) count = 16;
  if (count) memcpy(sources + 2, others, count * sizeof(struct pollfd));

  while (true) {
    i32 ready = poll(sources, 2 + count, timeoutMilliseconds);

    if (ready < 0) {
      if (errno == EINTR) continue;
      Fatal("cannot wait for input: %s", strerror(errno));
    }

    if (ready == 0) return TermWaitTimeout;

    // the other descriptors are only reported when the terminal has nothing, since their
    // owner looks at all of them on every pass anyway.
    if (!(sources[1].revents & POLLIN)) {
      return sources[0].revents ? TermWaitInput : TermWaitOther;
    }

    // several resize signals may have arrived, one redraw covers them all.
    char drain[64];
    while (read(wakePipe[0], drain, sizeof(drain)) > 0) {}
    return TermWaitResize;
  }
}

void
TermReadInput(void)
{
  char buffer[65536];
  i64 count = read(STDIN_FILENO, buffer, sizeof(buffer));

  if (count < 0 && (errno == EINTR || errno == EAGAIN)) return;

  if (count <= 0) {
    hungUp = 1;
    return;
  }

  InputFeed(buffer, (u64)count);
}

void
TermWrite(const char *data, u64 length)
{
  if (!WriteAll(data, length)) hungUp = 1;
}

void
TermSetTitle(const char *text, u64 length)
{
  if (length == title.length && (!length || memcmp(text, title.data, length) == 0)) return;

  StrBufClear(&title);
  StrBufAppend(&title, text, length);
  StrBuf sequence = {};
  StrBufAppendString(&sequence, "\x1b]2;");

  // a control character, c0 or c1 (as utf-8), would end or garble the sequence.
  for (u64 index = 0; index < length; index++) {
    u8 byte = (u8)text[index];
    bool c1 = byte == 0xC2 && index + 1 < length && (u8)text[index + 1] < 0xA0;

    if (byte < 0x20 || byte == 0x7F || c1) {
      StrBufAppendByte(&sequence, '?');
      if (c1) index++;
    } else {
      StrBufAppendByte(&sequence, (char)byte);
    }
  }

  StrBufAppendString(&sequence, "\x1b\\");
  TermWrite(sequence.data, sequence.length);
  StrBufFree(&sequence);
}

void
TermSetFolder(const char *path, u64 length)
{
  if (length == folder.length && (!length || memcmp(path, folder.data, length) == 0)) return;

  StrBufClear(&folder);
  StrBufAppend(&folder, path, length);
  StrBuf sequence = {};
  AppendFolder(&sequence, path, length);
  TermWrite(sequence.data, sequence.length);
  StrBufFree(&sequence);
}

bool
TermHungUp(void)
{
  return hungUp;
}
