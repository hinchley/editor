#include "scan.h"

#include <stdlib.h>
#include <string.h>

#include "memory.h"
#include "term.h"

// how long a scan runs before the window is drawn again, in milliseconds.
static constexpr i64 sliceTime = 20;

static Workspace *workspace;
static StrBuf *message;

// the scan in progress: the entries to go through and the next one.
static bool busy;
static u64 *queue;
static u64 queueCount;
static u64 queueNext;
static u64 skipped;
static const ScanClient *client;
static void *clientContext;

// a file read from disk, reused from one entry to the next.
static StrBuf readText;

void
ScanInit(Workspace *newWorkspace, StrBuf *newMessage)
{
  workspace = newWorkspace;
  message = newMessage;
}

void
ScanStart(const u64 *indexes, u64 count, const ScanClient *newClient, void *context)
{
  free(queue);
  queue = MemoryAllocate((count ? count : 1) * sizeof(u64));
  if (count) memcpy(queue, indexes, count * sizeof(u64));
  queueCount = count;
  queueNext = 0;
  skipped = 0;
  client = newClient;
  clientContext = context;
  busy = true;
}

bool
ScanBusy(void)
{
  return busy;
}

// ends the scan before telling the client, which may start another.
static void
Finish(bool stopped)
{
  ScanResult result = {queueNext, skipped, stopped};
  const ScanClient *finished = client;
  busy = false;
  free(queue);
  queue = nullptr;
  StrBufFree(&readText);
  finished->finish(clientContext, result);
}

// visits one entry, returning false when the visitor ends the scan.
static bool
ScanEntry(u64 index)
{
  const WorkspaceEntry *entry = &workspace->entries[index];
  ScanItem item = {.index = index, .document = entry->document};
  if (item.document) return client->visit(clientContext, &item);

  const char *path = entry->path.data ? entry->path.data : "";
  if (client->wants && !client->wants(clientContext, index, FileStampOf(path))) return true;

  if (FileRead(path, &readText, &item.stamp, nullptr) != FileStatusOk) {
    skipped++;
    return true;
  }

  item.text = readText.data;
  item.length = readText.length;
  return client->visit(clientContext, &item);
}

void
ScanContinue(void)
{
  i64 deadline = TermNow() + sliceTime;
  bool going = true;

  while (going && queueNext < queueCount) {
    going = ScanEntry(queue[queueNext++]);
    if (TermNow() > deadline) break;
  }

  if (!going || queueNext == queueCount) {
    Finish(false);
    return;
  }

  StrBufClear(message);
  client->progress(clientContext, queueNext, queueCount, message);
}

void
ScanKey(Key key)
{
  if (key.code == KeyEscape && !key.modifiers) Finish(true);
}
