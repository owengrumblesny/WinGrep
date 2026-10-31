#include <stdio.h>
#include <io.h>
#include <windows.h>
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