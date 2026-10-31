#ifndef COMPAT_H
#define COMPAT_H

int win_is_device (int fd);
long win_page_size (void);
int win_isatty (int fd);
int win_is_null_output (int fd);

#endif