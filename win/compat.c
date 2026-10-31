#include <config.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <io.h>
#include <windows.h>
#include <winternl.h>
#include "compat.h"
#include "fcntl-safer.h"
#include "filename.h"
#include "unistd-safer.h"
#include "xalloc.h"

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

int
win_openat (int dirfd, char const *name, int flags)
{
  char *joined = NULL;
  char const *path = name;
  WCHAR *wide;
  int wide_len;
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

  wide_len = MultiByteToWideChar (CP_ACP, 0, path, -1, NULL, 0);
  wide = xnmalloc (wide_len, sizeof *wide);
  MultiByteToWideChar (CP_ACP, 0, path, -1, wide, wide_len);

  h = CreateFileW (wide, GENERIC_READ,
                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                   NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
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
      errno = errno_from_win32 (err);
      return -1;
    }

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