#pragma once

// git: reads what a file looked like in the last commit of the git repository holding it.
// this is a reader for typical repositories only: a work tree with a .git folder, loose
// objects and packs (index version 2), loose and packed refs, and sha-1 or sha-256 object
// names.
// anything else (a .git file as in worktrees and submodules, reftable refs, bare
// repositories) is not a repository as far as the editor is concerned.
// objects are found by the names refs, commits and trees give, so nothing is ever hashed.
#include "strbuf.h"
#include "types.h"

// an object name, of 20 bytes (sha-1) or 32 (sha-256), the rest zero.
typedef struct GitHash {
  u8 bytes[32];
} GitHash;

// a repository, which lives until the editor exits.
typedef struct GitRepo GitRepo;

enum GitFile {
  GitFileOk,

  // the commit has no regular file at the path.
  GitFileUntracked,

  // an object could not be read: missing (a partial clone), corrupt or of an unknown
  // format.
  GitFileUnreadable
};

// the repository whose work tree holds the file at path (which need not exist), or
// nullptr when there is none or it is not a typical one.
// relative receives the file's path from the top of the work tree, with "/" between names.
GitRepo *
GitRepoOf(const char *path, StrBuf *relative);

// the commit HEAD names now, or false when it names none (a branch with no commits yet)
// or cannot be read.
// the refs are only read again when HEAD, the branch's ref file or packed-refs changed.
bool
GitHead(GitRepo *repo, GitHash *commit);

bool
GitHashEqual(const GitHash *a, const GitHash *b);

// reads the file at relative as it is in commit into text, replacing what it held.
enum GitFile
GitReadFile(GitRepo *repo, const GitHash *commit, const char *relative, StrBuf *text);
