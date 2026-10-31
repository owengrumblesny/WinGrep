#include <config.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <io.h>
#include <windows.h>
#include <winternl.h>
#include "compat.h"

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