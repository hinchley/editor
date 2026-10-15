#pragma once

// scan: goes through a list of loaded entries a slice at a time between frames, handing
// each to a visitor, so a long walk (project search, indexing symbols) shows its progress
// and escape stops it.
// an open entry is visited with its document, as it is in memory, and any other is read
// from disk without opening it, skipping binary, very large and unreadable files.
// one scan runs at a time, and while it runs it takes every key; no entry is added or
// removed meanwhile.
#include "document.h"
#include "file.h"
#include "key.h"
#include "strbuf.h"
#include "types.h"
#include "workspace.h"

// one entry of a scan: its index, and either its document or the text of its file.
typedef struct ScanItem {
  u64 index;
  const Document *document;
  const char *text;
  u64 length;

  // the version of the file read.
  FileStamp stamp;
} ScanItem;

// how a scan ended.
typedef struct ScanResult {
  // the entries gone through, and those of them that could not be read.
  u64 done;
  u64 skipped;

  // escape stopped it.
  bool stopped;
} ScanResult;

typedef struct ScanClient {
  // whether to read an entry that is not open, given the version of its file now (which
  // does not exist when it is gone), or nullptr to read every one.
  // an entry left unread is not counted as skipped.
  bool (*wants)(void *context, u64 index, FileStamp stamp);

  // takes one entry, returning false to end the scan early.
  bool (*visit)(void *context, const ScanItem *item);

  // appends the progress so far to message, after done of count entries.
  void (*progress)(void *context, u64 done, u64 count, StrBuf *message);

  // the scan is over, from the end of the list, a visit or escape.
  void (*finish)(void *context, ScanResult result);
} ScanClient;

// the scans run on workspace, and report their progress to message.
void
ScanInit(Workspace *workspace, StrBuf *message);

// starts a scan of the entries at indexes (copied), which goes through them in order once
// ScanContinue is called.
// client must stay valid until the scan finishes.
void
ScanStart(const u64 *indexes, u64 count, const ScanClient *client, void *context);

// true while a scan is running.
bool
ScanBusy(void);

// scans for a while, then returns so the window can be drawn and keys read.
void
ScanContinue(void);

// takes a key while a scan is running: escape stops it, and every other key is ignored.
void
ScanKey(Key key);
