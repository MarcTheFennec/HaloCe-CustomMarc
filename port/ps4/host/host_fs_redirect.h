/*
HOST_FS_REDIRECT.H

Included first (-include) when port/linux/src/posix_files.c is built into the
PS4 host: that file's path arguments are guest paths (/halo/...), which
these macros send through the content root resolution of host_files.c.
*/

#ifndef __HOST_FS_REDIRECT_H
#define __HOST_FS_REDIRECT_H

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <time.h>

int host_fs_stat(const char *path, struct stat *st);
int host_fs_chmod(const char *path, mode_t mode);
int host_fs_mkdir(const char *path, mode_t mode);
DIR *host_fs_opendir(const char *path);
int host_fs_utimensat(int dirfd, const char *path, const struct timespec times[2], int flags);
int host_fs_statvfs(const char *path, struct statvfs *st);

#define stat(path, st) host_fs_stat((path), (st))
#define chmod(path, mode) host_fs_chmod((path), (mode))
#define mkdir(path, mode) host_fs_mkdir((path), (mode))
#define opendir(path) host_fs_opendir((path))
#define utimensat(dirfd, path, times, flags) host_fs_utimensat((dirfd), (path), (times), (flags))
#define statvfs(path, st) host_fs_statvfs((path), (st))

#endif
