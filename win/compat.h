#ifndef COMPAT_H
#define COMPAT_H

#include <stdbool.h>
#include <sys/stat.h>
#include <wchar.h>

extern bool win_skip_hidden;
extern bool win_skip_offline;

void win_init (void);
void win_stack_guard (void);
void win_expand_operands (int *argc, char ***argv, int first);
bool win_stdout_closed (void);
int win_is_device (int fd);
long win_page_size (void);
int win_isatty (int fd);
int win_fstat (int fd, struct stat *st);
int win_openat (int dirfd, char const *name, int flags);
int win_is_null_output (int fd);

/* Shared with win/fts.c.  */
wchar_t *win_wide_path (char const *path);
char *win_utf8_from_wide (wchar_t const *wide);
int win_errno (unsigned long err);

#endif
