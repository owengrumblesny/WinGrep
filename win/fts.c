/* WinGrep Windows port.
   Copyright 2026 Owen Grumbles.
   Licensed under the GNU General Public License, version 3 or later.  */


/* A directory walker for grep -r, built on FindFirstFileExW.

   This replaces gnulib's fts.c with just the parts grep uses.  Each file's
   type comes from the directory listing, so nothing needs a separate stat,
   and no descriptors or changes of directory are involved, because every
   entry's fts_accpath is its full path.  Directories are listed completely
   before any of their entries are visited, as fts does.  */

#include <config.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include "compat.h"
#include "fts_.h"
#include "xalloc.h"

enum
{
  RECALL_ATTRIBUTES = (FILE_ATTRIBUTE_OFFLINE
                       | FILE_ATTRIBUTE_RECALL_ON_OPEN
                       | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS)
};

/* Unlisted reparse points, such as OneDrive placeholders and deduplicated
   files, are ordinary files that happen to have a tag.  */
#define IS_LINK_TAG(tag) \
  ((tag) == IO_REPARSE_TAG_SYMLINK || (tag) == IO_REPARSE_TAG_MOUNT_POINT)

struct file_id
{
  DWORD volume;
  ULONGLONG index;
};

struct frame
{
  FTSENT **entries;
  size_t count;
  size_t next;
  char const *path;
  struct file_id id;
  bool identified;
};

struct walker
{
  FTS fts;                      /* Must be first: callers hold an FTS *.  */
  FTSENT **roots;
  size_t root_count;
  size_t next_root;
  struct frame *frames;
  size_t depth;
  size_t frames_alloc;
};

static bool
identify_directory (char const *path, struct file_id *id)
{
  wchar_t *wide = win_wide_path (path);
  HANDLE h = CreateFileW (wide, 0,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  BY_HANDLE_FILE_INFORMATION info;
  bool ok = false;

  free (wide);
  if (h == INVALID_HANDLE_VALUE)
    return false;
  if (GetFileInformationByHandle (h, &info))
    {
      id->volume = info.dwVolumeSerialNumber;
      id->index = ((ULONGLONG) info.nFileIndexHigh << 32) | info.nFileIndexLow;
      ok = true;
    }
  CloseHandle (h);
  return ok;
}

/* Return true if the directory ID is one we are already inside.  Following
   links can lead back to a parent, and from there around forever.  IDs are
   looked up here, and only once a link turns up, because opening every
   folder would slow down trees that have no links at all.  */
static bool
is_active_directory (struct walker *w, struct file_id const *id)
{
  for (size_t i = 0; i < w->depth; i++)
    {
      struct frame *f = &w->frames[i];
      if (! f->identified)
        f->identified = identify_directory (f->path, &f->id);
      if (f->identified && f->id.volume == id->volume
          && f->id.index == id->index)
        return true;
    }
  return false;
}

static FTSENT *
new_entry (struct walker *w, char const *name, char const *parent_path,
           ptrdiff_t level)
{
  size_t namelen = strlen (name);
  FTSENT *ent = xzalloc (offsetof (FTSENT, fts_name) + namelen + 1);

  memcpy (ent->fts_name, name, namelen + 1);
  ent->fts_namelen = namelen;
  ent->fts_level = level;
  ent->fts_fts = &w->fts;
  ent->fts_instr = FTS_NOINSTR;

  if (parent_path)
    {
      size_t len = strlen (parent_path);
      bool slash = len && parent_path[len - 1] == '/';
      ent->fts_path = xmalloc (len + !slash + namelen + 1);
      memcpy (ent->fts_path, parent_path, len);
      if (! slash)
        ent->fts_path[len++] = '/';
      memcpy (ent->fts_path + len, name, namelen + 1);
    }
  else
    ent->fts_path = xstrdup (name);

  ent->fts_pathlen = strlen (ent->fts_path);
  ent->fts_accpath = ent->fts_path;
  return ent;
}

static void
free_entries (FTSENT **entries, size_t count)
{
  for (size_t i = 0; i < count; i++)
    {
      free (entries[i]->fts_path);
      free (entries[i]);
    }
  free (entries);
}

/* Fill in what fts_read reports for ENT, found with ATTRIBUTES and, if it is
   a reparse point, TAG.  Return false if the entry should be left out.  */
static bool
classify (struct walker *w, FTSENT *ent, DWORD attributes, DWORD tag)
{
  bool directory = attributes & FILE_ATTRIBUTE_DIRECTORY;
  bool link = attributes & FILE_ATTRIBUTE_REPARSE_POINT && IS_LINK_TAG (tag);

  if (win_skip_hidden
      && attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))
    return false;

  /* Opening one of these would download it from the cloud.  */
  if (win_skip_offline && ! directory && attributes & RECALL_ATTRIBUTES)
    return false;

  ent->fts_statp->st_mode = directory ? _S_IFDIR : _S_IFREG;

  //if (link && ! (w->fts.fts_options & FTS_LOGICAL))
if (link && tag == IO_REPARSE_TAG_SYMLINK
      && ! (w->fts.fts_options & FTS_LOGICAL))
    {
      ent->fts_info = FTS_SL;
      return true;
    }

  if (! directory)
    {
      ent->fts_info = FTS_F;
      return true;
    }

  ent->fts_info = FTS_D;
  if (link)
    {
      struct file_id id;
      if (! identify_directory (ent->fts_path, &id))
        ent->fts_info = FTS_SLNONE;
      else if (is_active_directory (w, &id))
        ent->fts_info = FTS_DC;
    }
  return true;
}

/* List directory ENT onto the stack of frames.  Return 0, or an errno value
   if it cannot be read.  */
static int
enter_directory (struct walker *w, FTSENT *ent)
{
  size_t pathlen = ent->fts_pathlen;
  char *pattern = xmalloc (pathlen + 3);
  wchar_t *wide;
  WIN32_FIND_DATAW found;
  HANDLE h;
  DWORD err = 0;
  //struct frame frame = { NULL, 0, 0, { 0, 0 } };
    struct frame frame = { .path = ent->fts_path }; 
	
	//reads files by path? idk. 
	//instead of doing it through null values like the original.
	
  size_t alloc = 0;

  memcpy (pattern, ent->fts_path, pathlen);
  if (pathlen && ent->fts_path[pathlen - 1] != '/'
      && ent->fts_path[pathlen - 1] != '\\')
    pattern[pathlen++] = '\\';
  strcpy (pattern + pathlen, "*");
  wide = win_wide_path (pattern);
  free (pattern);

  h = FindFirstFileExW (wide, FindExInfoBasic, &found, FindExSearchNameMatch,
                        NULL, FIND_FIRST_EX_LARGE_FETCH);
  if (h == INVALID_HANDLE_VALUE)
    err = GetLastError ();
  free (wide);
  if (err)
    return win_errno (err);

  /* Go on the stack before listing, so that a link inside this directory
     pointing back at it is recognized.  */
  if (w->depth == w->frames_alloc)
    w->frames = x2nrealloc (w->frames, &w->frames_alloc, sizeof *w->frames);
  w->frames[w->depth++] = frame;

  
  do
    {
      char *name;
      FTSENT *child;

      if (wcscmp (found.cFileName, L".") == 0
          || wcscmp (found.cFileName, L"..") == 0)
        continue;

      name = win_utf8_from_wide (found.cFileName);
      child = new_entry (w, name, ent->fts_path, ent->fts_level + 1);
      free (name);

      if (! classify (w, child, found.dwFileAttributes, found.dwReserved0))
        {
          free (child->fts_path);
          free (child);
          continue;
        }

      if (frame.count == alloc)
        frame.entries = x2nrealloc (frame.entries, &alloc,
                                    sizeof *frame.entries);
      frame.entries[frame.count++] = child;
    }
  while (FindNextFileW (h, &found));

  err = GetLastError ();
  FindClose (h);
  if (err != ERROR_NO_MORE_FILES)
    {
      w->depth--;
      free_entries (frame.entries, frame.count);
      return win_errno (err);
    }

/* Folder IDs are looked up by is_active_directory, and only once a link
   turns up.  Opening every folder here would slow down trees that have
   no links at all.  */

  w->frames[w->depth - 1].entries = frame.entries;
  w->frames[w->depth - 1].count = frame.count;
return 0;
}

FTS *
fts_open (char *const *argv, int options,
          int (*compare) (FTSENT const **, FTSENT const **))
{
  struct walker *w = xzalloc (sizeof *w);
  size_t count = 0;

  (void) compare;
  w->fts.fts_options = options;
  w->fts.fts_cwd_fd = AT_FDCWD;

  while (argv[count])
    count++;
  w->roots = xnmalloc (count, sizeof *w->roots);
  w->root_count = count;

  for (size_t i = 0; i < count; i++)
    {
      /* Like fts, reduce any trailing slashes to one.  */
      char *name = xstrdup (argv[i]);
      size_t len = strlen (name);
      while (2 < len && name[len - 1] == '/' && name[len - 2] == '/')
        name[--len] = '\0';

      FTSENT *ent = new_entry (w, name, NULL, FTS_ROOTLEVEL);
      free (name);
      wchar_t *wide = win_wide_path (ent->fts_path);
      DWORD attributes = GetFileAttributesW (wide);
      free (wide);

      if (attributes == INVALID_FILE_ATTRIBUTES)
        {
          ent->fts_info = FTS_NS;
          ent->fts_errno = win_errno (GetLastError ());
        }
      else
        {
          /* A link named on the command line has already been followed.  */
          bool directory = attributes & FILE_ATTRIBUTE_DIRECTORY;
          ent->fts_info = directory ? FTS_D : FTS_F;
          ent->fts_statp->st_mode = directory ? _S_IFDIR : _S_IFREG;
        }
      w->roots[i] = ent;
    }

  return &w->fts;
}

FTSENT *
fts_read (FTS *fts)
{
  struct walker *w = (struct walker *) fts;
  FTSENT *cur = fts->fts_cur;

  /* The caller has seen CUR, so now is the time to go inside it, unless
     the caller used fts_set to say otherwise.  */
  if (cur && cur->fts_info == FTS_D && cur->fts_instr != FTS_SKIP)
    {
      int err = enter_directory (w, cur);
      if (err)
        {
          cur->fts_info = FTS_DNR;
          cur->fts_errno = err;
          return cur;
        }
    }

  for (;;)
    {
      if (w->depth)
        {
          struct frame *top = &w->frames[w->depth - 1];
          if (top->next < top->count)
            {
              fts->fts_cur = top->entries[top->next++];
              return fts->fts_cur;
            }
          free_entries (top->entries, top->count);
          w->depth--;
        }
      else if (w->next_root < w->root_count)
        {
          fts->fts_cur = w->roots[w->next_root++];
          /* Remember the root so that links back to it are caught.  */
          struct frame root = { .path = fts->fts_cur->fts_path };
          if (w->depth == w->frames_alloc)
            w->frames = x2nrealloc (w->frames, &w->frames_alloc,
                                    sizeof *w->frames);
          w->frames[w->depth++] = root;
          return fts->fts_cur;
        }
      else
        {
          fts->fts_cur = NULL;
          errno = 0;
          return NULL;
        }
    }
}

int
fts_set (FTS *fts, FTSENT *ent, int instruction)
{
  (void) fts;
  ent->fts_instr = instruction;
  return 0;
}

int
fts_close (FTS *fts)
{
  struct walker *w = (struct walker *) fts;

  while (w->depth)
    {
      struct frame *top = &w->frames[--w->depth];
      
        free_entries (top->entries, top->count);
    }
  free (w->frames);
  free_entries (w->roots, w->root_count);
  free (w);
  return 0;
}
