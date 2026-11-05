/* Callback declarations for local unit tests without libfuse.
 * Linux CI also compiles against real libfuse and performs mounted tests. */
#ifndef RUFS_FUSE_STUB_H
#define RUFS_FUSE_STUB_H
#include <stdint.h>
#include <sys/stat.h>
#include <time.h>
struct fuse_conn_info {int unused;};
struct fuse_file_info {int unused;};
typedef int (*fuse_fill_dir_t)(void *,const char *,const struct stat *,off_t);
struct fuse_operations {
 void *(*init)(struct fuse_conn_info *);
 void (*destroy)(void *);
 int (*getattr)(const char *,struct stat *);
 int (*readdir)(const char *,void *,fuse_fill_dir_t,off_t,struct fuse_file_info *);
 int (*opendir)(const char *,struct fuse_file_info *);
 int (*mkdir)(const char *,mode_t);
 int (*create)(const char *,mode_t,struct fuse_file_info *);
 int (*open)(const char *,struct fuse_file_info *);
 int (*read)(const char *,char *,size_t,off_t,struct fuse_file_info *);
 int (*write)(const char *,const char *,size_t,off_t,struct fuse_file_info *);
 int (*rmdir)(const char *);
 int (*releasedir)(const char *,struct fuse_file_info *);
 int (*unlink)(const char *);
 int (*truncate)(const char *,off_t);
 int (*flush)(const char *,struct fuse_file_info *);
 int (*utimens)(const char *,const struct timespec [2]);
 int (*release)(const char *,struct fuse_file_info *);
};
#endif
