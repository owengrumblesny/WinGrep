#include <config.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>
#include <io.h>
#include <windows.h>
#include <winternl.h>
#include "colorize.h"
#include "compat.h"
#include "fcntl-safer.h"
#include "filename.h"
#include "unistd-safer.h"
#include "xalloc.h"

bool win_skip_hidden;
bool win_skip_offline;

static UINT saved_console_cp;
static DWORD saved_console_mode;
static HANDLE vt_console = INVALID_HANDLE_VALUE;

static void
restore_console (void)
{
  if (saved_console_cp)
    SetConsoleOutputCP (saved_console_cp);
  if (vt_console != INVALID_HANDLE_VALUE)
    SetConsoleMode (vt_console, saved_console_mode);
}

/* Ctrl+C ends the process without running atexit handlers.  */
static BOOL WINAPI
console_ctrl_handler (DWORD type)
{
  (void) type;
  restore_console ();
  ExitProcess (130);
}

/* The console code page and mode belong to the terminal and outlive this
   process, so put them back on the way out, however that happens.  */
void
win_init (void)
{
  saved_console_cp = GetConsoleOutputCP ();
  if (saved_console_cp && SetConsoleOutputCP (CP_UTF8))
    {
      atexit (restore_console);
      SetConsoleCtrlHandler (console_ctrl_handler, TRUE);
    }
  else
    saved_console_cp = 0;
}

static LONG WINAPI
stack_overflow_handler (EXCEPTION_POINTERS *info)
{
  static char const message[] = ": stack overflow\n";
  char const *name = getprogname ();
  HANDLE err = GetStdHandle (STD_ERROR_HANDLE);
  DWORD wrote;

  if (info->ExceptionRecord->ExceptionCode != EXCEPTION_STACK_OVERFLOW)
    return EXCEPTION_CONTINUE_SEARCH;

  WriteFile (err, name, strlen (name), &wrote, NULL);
  WriteFile (err, message, sizeof message - 1, &wrote, NULL);
  restore_console ();
  TerminateProcess (GetCurrentProcess (), 2);
  return EXCEPTION_CONTINUE_SEARCH;
}

/* A runaway regex overflows the stack.  Leave enough stack in reserve for
   the handler to report that, rather than dying without a word.  */
void
win_stack_guard (void)
{
  ULONG reserve = 64 * 1024;
  SetThreadStackGuarantee (&reserve);
  AddVectoredExceptionHandler (1, stack_overflow_handler);
}

/* Return true if the reader of a stdout pipe has gone away.  A zero-byte
   write is the one test that works: the C runtime reports this condition
   as either EPIPE or EINVAL, depending on when it is noticed.  */
bool
win_stdout_closed (void)
{
  HANDLE h = (HANDLE) _get_osfhandle (STDOUT_FILENO);
  DWORD wrote;

  if (h == INVALID_HANDLE_VALUE || GetFileType (h) != FILE_TYPE_PIPE)
    return false;
  if (WriteFile (h, "", 0, &wrote, NULL))
    return false;
  return (GetLastError () == ERROR_NO_DATA
          || GetLastError () == ERROR_BROKEN_PIPE);
}

int
should_colorize (void)
{
  char const *t = getenv ("TERM");
  return ! (t && strcmp (t, "dumb") == 0);
}

/* Modern consoles understand the same escape sequences as a Unix terminal,
   once asked to.  That gives grep its full range of colors.  */
void
init_colorize (void)
{
  HANDLE h = GetStdHandle (STD_OUTPUT_HANDLE);
  DWORD mode;

  if (GetConsoleMode (h, &mode)
      && SetConsoleMode (h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
    {
      vt_console = h;
      saved_console_mode = mode;
    }
}

void
print_start_colorize (char const *sgr_start, char const *sgr_seq)
{
  printf (sgr_start, sgr_seq);
}

void
print_end_colorize (char const *sgr_end)
{
  fputs (sgr_end, stdout);
}

long
win_page_size (void)
{
  SYSTEM_INFO info;
  GetSystemInfo (&info);
  return info.dwPageSize;
}

int
win_isatty (int fd)

{
  HANDLE h = (HANDLE) _get_osfhandle (fd);
  DWORD mode;
  int result;

  if (h == INVALID_HANDLE_VALUE)
    return 0;
  result = GetConsoleMode (h, &mode) != 0;
  return result;
}


int
win_is_device (int fd)
{
  HANDLE h = (HANDLE) _get_osfhandle (fd);
  DWORD type;
  int result;

  if (h == INVALID_HANDLE_VALUE)
    return 0;
  type = GetFileType (h);
  result = type == FILE_TYPE_CHAR || type == FILE_TYPE_PIPE;
  return result;
}

int
win_fstat (int fd, struct stat *st)
{
  HANDLE h = (HANDLE) _get_osfhandle (fd);
  BY_HANDLE_FILE_INFORMATION info;
  DWORD type;

  /* Windows cannot open a directory as a descriptor, so gnulib's open
     returns a dummy one and remembers the name.  Only gnulib can stat it.  */
  if (_gl_directory_name (fd))
    return fstat (fd, st);

  if (h == INVALID_HANDLE_VALUE)
    {
      errno = EBADF;
      return -1;
    }

  memset (st, 0, sizeof *st);
  type = GetFileType (h);
  if (type == FILE_TYPE_CHAR || type == FILE_TYPE_PIPE)
    {
      st->st_mode = type == FILE_TYPE_CHAR ? _S_IFCHR : _S_IFIFO;
      return 0;
    }

  if (! GetFileInformationByHandle (h, &info))
    {
      errno = GetLastError () == ERROR_ACCESS_DENIED ? EACCES : EIO;
      return -1;
    }

  st->st_mode = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY
                 ? _S_IFDIR : _S_IFREG);
  st->st_size = ((off_t) info.nFileSizeHigh << 32) | info.nFileSizeLow;
  st->st_nlink = info.nNumberOfLinks;
  st->st_dev = info.dwVolumeSerialNumber;
  st->st_ino = ((ino_t) info.nFileIndexHigh << 32) | info.nFileIndexLow;
  return 0;
}

static int
errno_from_win32 (DWORD err)
{
  switch (err)
    {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_BAD_PATHNAME:
    case ERROR_INVALID_DRIVE:
    case ERROR_BAD_NETPATH:
    case ERROR_BAD_NET_NAME:
      return ENOENT;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
      return EACCES;
    case ERROR_FILENAME_EXCED_RANGE:
      return ENAMETOOLONG;
    case ERROR_TOO_MANY_OPEN_FILES:
      return EMFILE;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
      return ENOMEM;
    default:
      /* The C runtime's open reports anything else this way too.  */
      return EINVAL;
    }
}

static WCHAR *
wide_from_utf8 (char const *s)
{
  int len = MultiByteToWideChar (CP_ACP, 0, s, -1, NULL, 0);
  WCHAR *wide = xnmalloc (len, sizeof *wide);
  MultiByteToWideChar (CP_ACP, 0, s, -1, wide, len);
  return wide;
}

/* Return a path that works past MAX_PATH, taking ownership of WIDE.
   The longPathAware manifest setting only helps if the user has also turned
   it on in the registry, whereas a \\?\ prefix always works.  */
static WCHAR *
long_path (WCHAR *wide)
{
  enum { DIRECTORY_LIMIT = 248 };
  DWORD len;
  WCHAR *full, *result;

  if (wcslen (wide) < DIRECTORY_LIMIT || wcsncmp (wide, L"\\\\?\\", 4) == 0)
    return wide;

  len = GetFullPathNameW (wide, 0, NULL, NULL);
  if (! len)
    return wide;
  full = xnmalloc (len, sizeof *full);
  GetFullPathNameW (wide, len, full, NULL);
  result = xnmalloc (len + 8, sizeof *result);
  if (wcsncmp (full, L"\\\\", 2) == 0)
    {
      wcscpy (result, L"\\\\?\\UNC\\");
      wcscat (result, full + 2);
    }
  else
    {
      wcscpy (result, L"\\\\?\\");
      wcscat (result, full);
    }
  free (full);
  free (wide);
  return result;
}

wchar_t *
win_wide_path (char const *path)
{
  return long_path (wide_from_utf8 (path));
}

char *
win_utf8_from_wide (wchar_t const *wide)
{
  int len = WideCharToMultiByte (CP_ACP, 0, wide, -1, NULL, 0, NULL, NULL);
  char *s = xmalloc (len);
  WideCharToMultiByte (CP_ACP, 0, wide, -1, s, len, NULL, NULL);
  return s;
}

int
win_errno (unsigned long err)
{
  return errno_from_win32 (err);
}

/* Copy UTF-16 file H, whose read position is just past its byte order mark,
   into a temporary UTF-8 file.  Grep works on bytes and would otherwise see
   every other byte as NUL.  Return the new handle after closing H, or
   INVALID_HANDLE_VALUE, leaving H open, if the file cannot be converted.  */
static HANDLE
utf8_copy_of_utf16 (HANDLE h, bool big_endian)
{
  enum { MAX_CONVERTED_SIZE = 1 << 30 };
  LARGE_INTEGER size;
  WCHAR dir[MAX_PATH + 1], name[MAX_PATH + 1];
  WCHAR *text;
  char *utf8;
  DWORD got, wrote, units;
  int utf8_len;
  HANDLE tmp;

  if (! GetFileSizeEx (h, &size) || size.QuadPart > MAX_CONVERTED_SIZE)
    return INVALID_HANDLE_VALUE;

  text = xmalloc (size.QuadPart + sizeof (WCHAR));
  if (! ReadFile (h, text, size.QuadPart, &got, NULL))
    {
      free (text);
      return INVALID_HANDLE_VALUE;
    }
  units = got / sizeof (WCHAR);
  if (big_endian)
    for (DWORD i = 0; i < units; i++)
      text[i] = text[i] << 8 | text[i] >> 8;

  utf8_len = WideCharToMultiByte (CP_UTF8, 0, text, units, NULL, 0, NULL, NULL);
  utf8 = xmalloc (utf8_len + 1);
  WideCharToMultiByte (CP_UTF8, 0, text, units, utf8, utf8_len, NULL, NULL);
  free (text);

  if (! GetTempPathW (MAX_PATH + 1, dir)
      || ! GetTempFileNameW (dir, L"wgr", 0, name))
    {
      free (utf8);
      return INVALID_HANDLE_VALUE;
    }
  tmp = CreateFileW (name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                     CREATE_ALWAYS,
                     FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                     NULL);
  if (tmp == INVALID_HANDLE_VALUE
      || ! WriteFile (tmp, utf8, utf8_len, &wrote, NULL)
      || SetFilePointer (tmp, 0, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER)
    {
      if (tmp != INVALID_HANDLE_VALUE)
        CloseHandle (tmp);
      free (utf8);
      return INVALID_HANDLE_VALUE;
    }
  free (utf8);
  CloseHandle (h);
  return tmp;
}

/* Deal with a byte order mark at the start of H.  Leave the read position
   past a UTF-8 mark so that ^ matches the first line, and swap UTF-16 for a
   UTF-8 copy.  -U asks for the raw bytes, so it skips all of this.  */
static HANDLE
handle_byte_order_mark (HANDLE h, int flags)
{
  unsigned char mark[3];
  DWORD got;

  if (flags & O_BINARY || GetFileType (h) != FILE_TYPE_DISK
      || ! ReadFile (h, mark, sizeof mark, &got, NULL))
    return h;

  if (got == 3 && mark[0] == 0xEF && mark[1] == 0xBB && mark[2] == 0xBF)
    return h;

  if (2 <= got && ((mark[0] == 0xFF && mark[1] == 0xFE)
                   || (mark[0] == 0xFE && mark[1] == 0xFF)))
    {
      HANDLE copy;
      SetFilePointer (h, 2, NULL, FILE_BEGIN);
      copy = utf8_copy_of_utf16 (h, mark[0] == 0xFE);
      if (copy != INVALID_HANDLE_VALUE)
        return copy;
    }

  SetFilePointer (h, 0, NULL, FILE_BEGIN);
  return h;
}

int
win_openat (int dirfd, char const *name, int flags)
{
  char *joined = NULL;
  char const *path = name;
  WCHAR *wide;
  int fd;
  HANDLE h;
  DWORD err;

  if (strcmp (name, "/dev/null") == 0)
    path = "NUL";
  else if (dirfd != AT_FDCWD && ! IS_ABSOLUTE_FILE_NAME (name))
    {
      char const *dir = _gl_directory_name (dirfd);
      if (! dir)
        return openat_safer (dirfd, name, flags);
      joined = xmalloc (strlen (dir) + strlen (name) + 2);
      stpcpy (stpcpy (stpcpy (joined, dir), "\\"), name);
      path = joined;
    }

  wide = win_wide_path (path);

  h = CreateFileW (wide, GENERIC_READ,
                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                   NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
  err = GetLastError ();
  free (wide);
  free (joined);

  if (h == INVALID_HANDLE_VALUE)
    {
      /* CreateFileW also says "access denied" for a directory.  gnulib
         turns that case into a dummy descriptor, and sets errno for a
         file that really is denied.  */
      if (err == ERROR_ACCESS_DENIED)
        return openat_safer (dirfd, name, flags);
      /* A wildcard that matched nothing is a missing file, not a bad name.  */
      errno = strpbrk (name, "*?") ? ENOENT : errno_from_win32 (err);
      return -1;
    }

  h = handle_byte_order_mark (h, flags);
  fd = _open_osfhandle ((intptr_t) h,
                        _O_RDONLY | (flags & O_BINARY ? _O_BINARY : _O_TEXT));
  if (fd < 0)
    {
      CloseHandle (h);
      errno = EMFILE;
      return -1;
    }
  return fd_safer (fd);
}

static int
compare_names (void const *a, void const *b)
{
  return CompareStringOrdinal (*(WCHAR *const *) a, -1,
                               *(WCHAR *const *) b, -1, TRUE) - CSTR_EQUAL;
}

/* Append to *LIST the files matching the wildcard PATTERN, keeping any
   directory part of it.  Return the number of files added.  */
static size_t
expand_wildcard (char const *pattern, char ***list, size_t *n, size_t *alloc)
{
  WCHAR *wide = wide_from_utf8 (pattern);
  WIN32_FIND_DATAW found;
  HANDLE h = FindFirstFileExW (wide, FindExInfoBasic, &found,
                               FindExSearchNameMatch, NULL,
                               FIND_FIRST_EX_LARGE_FETCH);
  WCHAR **names = NULL;
  size_t count = 0, names_alloc = 0;
  char const *base = pattern;
  char const *p;

  free (wide);
  if (h == INVALID_HANDLE_VALUE)
    return 0;

  for (p = pattern; *p; p++)
    if (*p == '/' || *p == '\\' || (*p == ':' && p == pattern + 1))
      base = p + 1;

  do
    {
      if (wcscmp (found.cFileName, L".") == 0
          || wcscmp (found.cFileName, L"..") == 0
          || found.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN
                                       | FILE_ATTRIBUTE_SYSTEM))
        continue;
      if (count == names_alloc)
        names = x2nrealloc (names, &names_alloc, sizeof *names);
      names[count++] = xmemdup (found.cFileName,
                                (wcslen (found.cFileName) + 1)
                                * sizeof (WCHAR));
    }
  while (FindNextFileW (h, &found));
  FindClose (h);

  qsort (names, count, sizeof *names, compare_names);

  for (size_t i = 0; i < count; i++)
    {
      char *name = win_utf8_from_wide (names[i]);
      size_t dirlen = base - pattern;
      char *full = xmalloc (dirlen + strlen (name) + 1);
      memcpy (full, pattern, dirlen);
      strcpy (full + dirlen, name);
      free (name);
      free (names[i]);
      if (*n == *alloc)
        *list = x2nrealloc (*list, alloc, sizeof **list);
      (*list)[(*n)++] = full;
    }
  free (names);
  return count;
}

/* Windows shells leave wildcards for the program to expand.  Expand them
   here rather than at startup, so that only file names are affected and
   the search pattern or --include=*.c are left alone.  */
void
win_expand_operands (int *argc, char ***argv, int first)
{
  char **list = NULL;
  size_t n = 0, alloc = 0;

  for (int i = 0; i < *argc; i++)
    {
      char *arg = (*argv)[i];
      if (! (first <= i && strpbrk (arg, "*?")
             && expand_wildcard (arg, &list, &n, &alloc)))
        {
          if (n == alloc)
            list = x2nrealloc (list, &alloc, sizeof *list);
          list[n++] = arg;
        }
    }

  if (n == alloc)
    list = x2nrealloc (list, &alloc, sizeof *list);
  list[n] = NULL;
  *argc = n;
  *argv = list;
}

int
win_is_null_output (int fd)
{
  static WCHAR const null_name[] = L"\\Device\\Null";
  HANDLE h = (HANDLE) _get_osfhandle (fd);
  union { OBJECT_NAME_INFORMATION info; char buf[1024]; } name;
  ULONG len;
  int result;

  if (h == INVALID_HANDLE_VALUE || GetFileType (h) != FILE_TYPE_CHAR)
    return 0;

  /* GetConsoleMode cannot do this: it also fails on a write-only console
     handle, such as stdout after "> CON".  1 is ObjectNameInformation.  */
  if (NtQueryObject (h, (OBJECT_INFORMATION_CLASS) 1, &name, sizeof name,
                     &len) < 0)
    return 0;

  result = CompareStringOrdinal (name.info.Name.Buffer,
                                 name.info.Name.Length / sizeof (WCHAR),
                                 null_name, -1, FALSE) == CSTR_EQUAL;
  return result;
}