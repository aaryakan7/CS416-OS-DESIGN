/* Copyright (C) 2025 CS416 Rutgers CS.
 * RUFS coursework by Aarya Dalal and Aarya Kansara. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define FUSE_USE_VERSION 26
#ifdef RUFS_TEST_STUB
#include "tests/fuse_stub.h"
#else
#include <fuse.h>
#endif
#include "block.h"
#include "rufs.h"
#include <errno.h>
#include <stdbool.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define INODES_PER_BLOCK (BLOCK_SIZE / sizeof(struct inode))
#define DIRENTS_PER_BLOCK (BLOCK_SIZE / sizeof(struct dirent))
#define MAX_FILE_SIZE (DIRECT_COUNT * BLOCK_SIZE)
_Static_assert(sizeof(struct inode) <= BLOCK_SIZE, "Inodes must fit inside a block");
_Static_assert(MAX_INUM <= BLOCK_SIZE * 8 && MAX_DNUM <= BLOCK_SIZE * 8,
               "Each allocation bitmap must fit inside a block");

union disk_block {
    unsigned char bytes[BLOCK_SIZE];
    struct superblock super;
    struct dirent entries[DIRENTS_PER_BLOCK];
};
char diskfile_path[PATH_MAX];
struct superblock sb;

static int valid_data_block(int number) {
    return number >= (int)sb.d_start_blk &&
           (uint32_t)number < sb.d_start_blk + sb.max_dnum;
}

static int allocate_bit(uint32_t bitmap_block, unsigned count, unsigned start) {
    union disk_block buffer;
    int error = bio_read((int)bitmap_block, buffer.bytes);
    if (error < 0) return error;
    for (unsigned i = start; i < count; i++) {
        if (!get_bitmap(buffer.bytes, (int)i)) {
            set_bitmap(buffer.bytes, (int)i);
            error = bio_write((int)bitmap_block, buffer.bytes);
            return error < 0 ? error : (int)i;
        }
    }
    return -ENOSPC;
}

static int release_bit(uint32_t bitmap_block, unsigned index) {
    union disk_block buffer;
    int error = bio_read((int)bitmap_block, buffer.bytes);
    if (error < 0) return error;
    unset_bitmap(buffer.bytes, (int)index);
    error = bio_write((int)bitmap_block, buffer.bytes);
    return error < 0 ? error : 0;
}

int get_avail_ino(void) {
    return allocate_bit(sb.i_bitmap_blk, sb.max_inum, 1);
}

int get_avail_blkno(void) {
    int index = allocate_bit(sb.d_bitmap_blk, sb.max_dnum, 0);
    return index < 0 ? index : (int)sb.d_start_blk + index;
}

static int release_block(int block) {
    if (!valid_data_block(block)) return -EIO;
    return release_bit(sb.d_bitmap_blk, (unsigned)block - sb.d_start_blk);
}

int readi(uint16_t ino, struct inode *inode) {
    if (!inode || ino >= sb.max_inum) return -EINVAL;
    union disk_block buffer;
    int error = bio_read((int)(sb.i_start_blk + ino / INODES_PER_BLOCK), buffer.bytes);
    if (error < 0) return error;
    memcpy(inode, buffer.bytes + (ino % INODES_PER_BLOCK) * sizeof(*inode), sizeof(*inode));
    return 0;
}

int writei(uint16_t ino, struct inode *inode) {
    if (!inode || ino >= sb.max_inum) return -EINVAL;
    union disk_block buffer;
    int block = (int)(sb.i_start_blk + ino / INODES_PER_BLOCK);
    int error = bio_read(block, buffer.bytes);
    if (error < 0) return error;
    memcpy(buffer.bytes + (ino % INODES_PER_BLOCK) * sizeof(*inode), inode, sizeof(*inode));
    error = bio_write(block, buffer.bytes);
    return error < 0 ? error : 0;
}

static int valid_entry(const struct dirent *entry) {
    return entry->ino < sb.max_inum && entry->len > 0 &&
           entry->len < sizeof(entry->name) &&
           strnlen(entry->name, sizeof(entry->name)) == entry->len;
}

int dir_find(uint16_t ino, const char *name, size_t length, struct dirent *result) {
    if (!name || !result || !length) return -EINVAL;
    if (length >= NAME_CAPACITY) return -ENAMETOOLONG;
    struct inode directory;
    int error = readi(ino, &directory);
    if (error < 0) return error;
    if (!directory.valid) return -ENOENT;
    if (!S_ISDIR(directory.type)) return -ENOTDIR;
    union disk_block buffer;
    for (int i = 0; i < DIRECT_COUNT; i++) {
        int block = directory.direct_ptr[i];
        if (!block) continue;
        if (!valid_data_block(block)) return -EIO;
        error = bio_read(block, buffer.bytes);
        if (error < 0) return error;
        for (size_t j = 0; j < DIRENTS_PER_BLOCK; j++) {
            struct dirent *entry = &buffer.entries[j];
            if (!entry->valid) continue;
            if (!valid_entry(entry)) return -EIO;
            if (entry->len == length && !memcmp(entry->name, name, length)) {
                *result = *entry;
                return 0;
            }
        }
    }
    return -ENOENT;
}

int dir_add(struct inode directory, uint16_t ino, const char *name, size_t length) {
    if (!name || !length || memchr(name, '/', length)) return -EINVAL;
    if (length >= NAME_CAPACITY) return -ENAMETOOLONG;
    int error = readi(directory.ino, &directory);
    if (error < 0) return error;
    if (!directory.valid) return -ENOENT;
    if (!S_ISDIR(directory.type)) return -ENOTDIR;
    struct dirent found;
    error = dir_find(directory.ino, name, length, &found);
    if (!error) return -EEXIST;
    if (error != -ENOENT) return error;
    struct inode child;
    error = readi(ino, &child);
    if (error < 0) return error;
    if (!child.valid) return -ENOENT;
    union disk_block before, after;
    int free_pointer = -1, slot = -1;
    size_t entry_index = 0;
    for (int i = 0; i < DIRECT_COUNT; i++) {
        if (!directory.direct_ptr[i]) {
            if (free_pointer < 0) free_pointer = i;
            continue;
        }
        error = bio_read(directory.direct_ptr[i], after.bytes);
        if (error < 0) return error;
        for (size_t j = 0; j < DIRENTS_PER_BLOCK; j++) {
            if (!after.entries[j].valid) {
                slot = i;
                entry_index = j;
                break;
            }
        }
        if (slot >= 0) break;
    }
    bool new_block = slot < 0;
    int block;
    if (new_block) {
        if (free_pointer < 0) return -ENOSPC;
        block = get_avail_blkno();
        if (block < 0) return block;
        slot = free_pointer;
        memset(&after, 0, sizeof(after));
    } else {
        block = directory.direct_ptr[slot];
        before = after;
    }
    struct dirent *entry = &after.entries[entry_index];
    *entry = (struct dirent){.ino = ino, .valid = 1, .len = (uint16_t)length};
    memcpy(entry->name, name, length);
    struct inode original = directory;
    if (new_block) {
        directory.direct_ptr[slot] = block;
        directory.size += BLOCK_SIZE;
    }
    bool special = (length == 1 && name[0] == '.') ||
                   (length == 2 && !memcmp(name, "..", 2));
    if (!special && S_ISDIR(child.type)) directory.link++;
    directory.vstat.st_mtime = directory.vstat.st_ctime = time(NULL);
    error = bio_write(block, after.bytes);
    if (error >= 0) error = writei(directory.ino, &directory);
    if (error < 0) {
        /* Best-effort restoration on I/O errors; normal ENOSPC paths are atomic. */
        writei(original.ino, &original);
        if (new_block) release_block(block);
        else bio_write(block, before.bytes);
        return error;
    }
    return 0;
}

int get_node_by_path(const char *path, uint16_t ino, struct inode *inode) {
    if (!path || !inode || path[0] != '/') return -EINVAL;
    size_t length = strnlen(path, PATH_MAX);
    if (length >= PATH_MAX) return -ENAMETOOLONG;
    struct inode current;
    int error = readi(ino, &current);
    if (error < 0) return error;
    if (!current.valid) return -ENOENT;
    char *copy = strdup(path);
    if (!copy) return -ENOMEM;
    char *save = NULL;
    for (char *part = strtok_r(copy, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        struct dirent entry;
        error = dir_find(current.ino, part, strlen(part), &entry);
        if (error < 0) break;
        error = readi(entry.ino, &current);
        if (error < 0) break;
        if (!current.valid || current.ino != entry.ino) { error = -EIO; break; }
    }
    free(copy);
    if (error < 0) return error;
    if (path[length - 1] == '/' && !S_ISDIR(current.type)) return -ENOTDIR;
    *inode = current;
    return 0;
}

static void initialize_inode(struct inode *inode, uint16_t ino, mode_t mode) {
    memset(inode, 0, sizeof(*inode));
    inode->ino = ino;
    inode->valid = 1;
    inode->type = mode;
    inode->link = S_ISDIR(mode) ? 2 : 1;
    inode->vstat.st_uid = getuid();
    inode->vstat.st_gid = getgid();
    inode->vstat.st_atime = inode->vstat.st_mtime = inode->vstat.st_ctime = time(NULL);
}

static void directory_entries(union disk_block *buffer, uint16_t self, uint16_t parent) {
    memset(buffer, 0, sizeof(*buffer));
    buffer->entries[0] = (struct dirent){.ino = self, .valid = 1, .name = ".", .len = 1};
    buffer->entries[1] = (struct dirent){.ino = parent, .valid = 1, .name = "..", .len = 2};
}

int rufs_mkfs(void) {
    int error = dev_init(diskfile_path);
    if (error < 0) return error;
    memset(&sb, 0, sizeof(sb));
    sb.magic_num = MAGIC_NUM;
    sb.max_inum = MAX_INUM;
    sb.i_bitmap_blk = 1;
    sb.d_bitmap_blk = 2;
    sb.i_start_blk = 3;
    sb.d_start_blk = 3 + (MAX_INUM + INODES_PER_BLOCK - 1) / INODES_PER_BLOCK;
    if (sb.d_start_blk >= DISK_BLOCKS) return -ENOSPC;
    unsigned data_blocks = DISK_BLOCKS - sb.d_start_blk;
    sb.max_dnum = (uint16_t)(data_blocks < MAX_DNUM ? data_blocks : MAX_DNUM);
    union disk_block buffer = {0};
    set_bitmap(buffer.bytes, 0);
    error = bio_write((int)sb.i_bitmap_blk, buffer.bytes);
    if (error < 0) return error;
    error = bio_write((int)sb.d_bitmap_blk, buffer.bytes);
    if (error < 0) return error;
    directory_entries(&buffer, 0, 0);
    error = bio_write((int)sb.d_start_blk, buffer.bytes);
    if (error < 0) return error;
    struct inode root;
    initialize_inode(&root, 0, S_IFDIR | 0755);
    root.size = BLOCK_SIZE;
    root.direct_ptr[0] = (int)sb.d_start_blk;
    error = writei(0, &root);
    if (error < 0) return error;
    /* The superblock is serialized into a complete, zero-padded disk block. */
    memset(&buffer, 0, sizeof(buffer));
    buffer.super = sb;
    error = bio_write(0, buffer.bytes);
    return error < 0 ? error : dev_sync();
}

static int load_filesystem(void) {
    int error = dev_open(diskfile_path);
    if (error == -ENOENT) return rufs_mkfs();
    if (error < 0) return error;
    union disk_block buffer;
    error = bio_read(0, buffer.bytes);
    if (error < 0) return error;
    struct superblock loaded = buffer.super;
    if (loaded.magic_num != MAGIC_NUM || !loaded.max_inum || loaded.max_inum > MAX_INUM ||
        !loaded.max_dnum || loaded.max_dnum > MAX_DNUM || loaded.i_bitmap_blk != 1 ||
        loaded.d_bitmap_blk != 2 || loaded.i_start_blk != 3) return -EINVAL;
    uint32_t expected_start = 3 + (loaded.max_inum + INODES_PER_BLOCK - 1) / INODES_PER_BLOCK;
    if (loaded.d_start_blk != expected_start || loaded.d_start_blk >= DISK_BLOCKS ||
        loaded.max_dnum > DISK_BLOCKS - loaded.d_start_blk) return -EINVAL;
    sb = loaded;
    error = bio_read((int)sb.i_bitmap_blk, buffer.bytes);
    if (error < 0) return error;
    if (!get_bitmap(buffer.bytes, 0)) return -EIO;
    error = bio_read((int)sb.d_bitmap_blk, buffer.bytes);
    if (error < 0) return error;
    if (!get_bitmap(buffer.bytes, 0)) return -EIO;
    struct inode root;
    error = readi(0, &root);
    if (error < 0) return error;
    if (!root.valid || root.ino != 0 || !S_ISDIR(root.type) ||
        root.size > MAX_FILE_SIZE || root.direct_ptr[0] != (int)sb.d_start_blk) return -EIO;
    return 0;
}

static void *rufs_init(struct fuse_conn_info *conn) {
    (void)conn;
    int error = load_filesystem();
    if (error < 0) {
        fprintf(stderr, "RUFS initialization failed: %s\n", strerror(-error));
        dev_close();
        exit(EXIT_FAILURE);
    }
    return NULL;
}

static void rufs_destroy(void *userdata) {
    (void)userdata;
    dev_close();
}

static int rufs_getattr(const char *path, struct stat *status) {
    struct inode inode;
    int error = get_node_by_path(path, 0, &inode);
    if (error < 0) return error;
    *status = inode.vstat;
    status->st_ino = inode.ino;
    status->st_mode = inode.type;
    status->st_nlink = inode.link;
    status->st_size = inode.size;
    status->st_blksize = BLOCK_SIZE;
    status->st_blocks = 0;
    for (int i = 0; i < DIRECT_COUNT; i++) {
        if (inode.direct_ptr[i]) status->st_blocks += BLOCK_SIZE / 512;
    }
    return 0;
}

static int rufs_opendir(const char *path, struct fuse_file_info *info) {
    (void)info;
    struct inode inode;
    int error = get_node_by_path(path, 0, &inode);
    if (error < 0) return error;
    return S_ISDIR(inode.type) ? 0 : -ENOTDIR;
}

static int rufs_readdir(const char *path, void *buffer, fuse_fill_dir_t filler,
                        off_t offset, struct fuse_file_info *info) {
    (void)info;
    if (offset < 0) return -EINVAL;
    struct inode inode;
    int error = get_node_by_path(path, 0, &inode);
    if (error < 0) return error;
    if (!S_ISDIR(inode.type)) return -ENOTDIR;
    size_t total = DIRECT_COUNT * DIRENTS_PER_BLOCK;
    if ((uint64_t)offset >= total) return 0;
    union disk_block block;
    for (size_t i = (size_t)offset / DIRENTS_PER_BLOCK; i < DIRECT_COUNT; i++) {
        if (!inode.direct_ptr[i]) continue;
        if (!valid_data_block(inode.direct_ptr[i])) return -EIO;
        error = bio_read(inode.direct_ptr[i], block.bytes);
        if (error < 0) return error;
        for (size_t j = 0; j < DIRENTS_PER_BLOCK; j++) {
            size_t cookie = i * DIRENTS_PER_BLOCK + j;
            if (cookie < (size_t)offset || !block.entries[j].valid) continue;
            if (!valid_entry(&block.entries[j])) return -EIO;
            /* A full caller buffer ends this batch successfully. */
            if (filler(buffer, block.entries[j].name, NULL, (off_t)(cookie + 1))) return 0;
        }
    }
    return 0;
}

static void discard_inode(struct inode *inode) {
    for (int i = 0; i < DIRECT_COUNT; i++) {
        if (inode->direct_ptr[i]) release_block(inode->direct_ptr[i]);
    }
    struct inode empty = {0};
    writei(inode->ino, &empty);
    release_bit(sb.i_bitmap_blk, inode->ino);
}

static int create_node(const char *path, mode_t mode, bool directory) {
    if (!path || path[0] != '/') return -EINVAL;
    size_t length = strnlen(path, PATH_MAX);
    if (length >= PATH_MAX) return -ENAMETOOLONG;
    if (length == 1) return -EEXIST;
    if (!directory && path[length - 1] == '/') return -EISDIR;
    char *copy = strdup(path);
    if (!copy) return -ENOMEM;
    while (length > 1 && copy[length - 1] == '/') copy[--length] = '\0';
    char *name = strrchr(copy, '/') + 1;
    size_t name_length = strlen(name);
    if (!name_length || !strcmp(name, ".") || !strcmp(name, "..")) {
        free(copy);
        return -EINVAL;
    }
    if (name_length >= NAME_CAPACITY) { free(copy); return -ENAMETOOLONG; }
    /* Keep the basename separate while terminating the parent path. */
    char basename[NAME_CAPACITY];
    memcpy(basename, name, name_length + 1);
    if (name == copy + 1) copy[1] = '\0';
    else name[-1] = '\0';
    struct inode parent;
    int error = get_node_by_path(copy, 0, &parent);
    free(copy);
    if (error < 0) return error;
    if (!S_ISDIR(parent.type)) return -ENOTDIR;
    struct dirent existing;
    error = dir_find(parent.ino, basename, name_length, &existing);
    if (!error) return -EEXIST;
    if (error != -ENOENT) return error;
    int ino = get_avail_ino();
    if (ino < 0) return ino;
    struct inode child;
    initialize_inode(&child, (uint16_t)ino, (directory ? S_IFDIR : S_IFREG) | (mode & 07777));
    if (directory) {
        int block = get_avail_blkno();
        if (block < 0) { discard_inode(&child); return block; }
        child.direct_ptr[0] = block;
        child.size = BLOCK_SIZE;
        union disk_block entries;
        directory_entries(&entries, child.ino, parent.ino);
        error = bio_write(block, entries.bytes);
        if (error < 0) { discard_inode(&child); return error; }
    }
    error = writei(child.ino, &child);
    if (error >= 0) error = dir_add(parent, child.ino, basename, name_length);
    if (error < 0) discard_inode(&child);
    return error;
}

static int rufs_mkdir(const char *path, mode_t mode) {
    return create_node(path, mode, true);
}

static int rufs_create(const char *path, mode_t mode, struct fuse_file_info *info) {
    (void)info;
    return create_node(path, mode, false);
}

static int rufs_open(const char *path, struct fuse_file_info *info) {
    (void)info;
    struct inode inode;
    int error = get_node_by_path(path, 0, &inode);
    if (error < 0) return error;
    return S_ISREG(inode.type) ? 0 : -EISDIR;
}

static int rufs_read(const char *path, char *buffer, size_t size, off_t offset,
                     struct fuse_file_info *info) {
    (void)info;
    if (offset < 0 || (!buffer && size)) return -EINVAL;
    struct inode inode;
    int error = get_node_by_path(path, 0, &inode);
    if (error < 0) return error;
    if (!S_ISREG(inode.type)) return -EISDIR;
    if (inode.size > MAX_FILE_SIZE) return -EIO;
    if (!size || (uint64_t)offset >= inode.size) return 0;
    if (size > inode.size - (uint64_t)offset) size = inode.size - (size_t)offset;
    size_t done = 0;
    union disk_block block;
    while (done < size) {
        size_t position = (size_t)offset + done;
        int logical = (int)(position / BLOCK_SIZE);
        size_t within = position % BLOCK_SIZE;
        size_t chunk = BLOCK_SIZE - within;
        if (chunk > size - done) chunk = size - done;
        int physical = inode.direct_ptr[logical];
        if (!physical) memset(buffer + done, 0, chunk);
        else {
            if (!valid_data_block(physical)) return -EIO;
            error = bio_read(physical, block.bytes);
            if (error < 0) return error;
            memcpy(buffer + done, block.bytes + within, chunk);
        }
        done += chunk;
    }
    inode.vstat.st_atime = time(NULL);
    error = writei(inode.ino, &inode);
    return error < 0 ? error : (int)done;
}

struct write_block {
    union disk_block before;
    union disk_block after;
    bool used;
    bool allocated;
};

static int rufs_write(const char *path, const char *buffer, size_t size, off_t offset,
                      struct fuse_file_info *info) {
    (void)info;
    if (offset < 0 || (!buffer && size)) return -EINVAL;
    struct inode inode;
    int error = get_node_by_path(path, 0, &inode);
    if (error < 0) return error;
    if (!S_ISREG(inode.type)) return -EISDIR;
    if (inode.size > MAX_FILE_SIZE) return -EIO;
    if (!size) return 0;
    if ((uint64_t)offset >= MAX_FILE_SIZE || size > MAX_FILE_SIZE - (uint64_t)offset) return -EFBIG;
    struct write_block *blocks = calloc(DIRECT_COUNT, sizeof(*blocks));
    if (!blocks) return -ENOMEM;
    struct inode original = inode;
    size_t start = (size_t)offset, end = start + size;
    for (int i = 0; i < DIRECT_COUNT; i++) {
        size_t lower = (size_t)i * BLOCK_SIZE, upper = lower + BLOCK_SIZE;
        bool target = lower < end && upper > start;
        bool gap = start > original.size && lower < start && upper > original.size;
        if (!target && !(gap && inode.direct_ptr[i])) continue;
        if (!inode.direct_ptr[i]) {
            int physical = get_avail_blkno();
            if (physical < 0) { error = physical; goto release; }
            inode.direct_ptr[i] = physical;
            blocks[i].allocated = true;
        } else {
            if (!valid_data_block(inode.direct_ptr[i])) { error = -EIO; goto release; }
            error = bio_read(inode.direct_ptr[i], blocks[i].before.bytes);
            if (error < 0) goto release;
            blocks[i].after = blocks[i].before;
        }
        blocks[i].used = true;
        if (gap) {
            size_t from = original.size > lower ? original.size - lower : 0;
            size_t to = start < upper ? start - lower : BLOCK_SIZE;
            memset(blocks[i].after.bytes + from, 0, to - from);
        }
        if (target) {
            size_t from = start > lower ? start : lower;
            size_t to = end < upper ? end : upper;
            memcpy(blocks[i].after.bytes + (from - lower), buffer + (from - start), to - from);
        }
    }
    for (int i = 0; i < DIRECT_COUNT; i++) {
        if (!blocks[i].used) continue;
        error = bio_write(inode.direct_ptr[i], blocks[i].after.bytes);
        if (error < 0) goto restore;
    }
    if (end > inode.size) inode.size = (uint32_t)end;
    inode.vstat.st_mtime = inode.vstat.st_ctime = time(NULL);
    error = writei(inode.ino, &inode);
    if (error < 0) goto restore;
    free(blocks);
    return (int)size;
restore:
    /* Restore old data and metadata before releasing newly reserved blocks. */
    for (int i = 0; i < DIRECT_COUNT; i++) {
        if (blocks[i].used && !blocks[i].allocated) bio_write(original.direct_ptr[i], blocks[i].before.bytes);
    }
    writei(original.ino, &original);
release:
    for (int i = 0; i < DIRECT_COUNT; i++) {
        if (blocks[i].allocated) release_block(inode.direct_ptr[i]);
    }
    free(blocks);
    return error;
}

/* Deletion and truncation are outside this coursework's required operations. */
static int rufs_rmdir(const char *path) { (void)path; return -EOPNOTSUPP; }
static int rufs_unlink(const char *path) { (void)path; return -EOPNOTSUPP; }
static int rufs_truncate(const char *path, off_t size) { (void)path; (void)size; return -EOPNOTSUPP; }
static int rufs_releasedir(const char *path, struct fuse_file_info *info) { (void)path; (void)info; return 0; }
static int rufs_release(const char *path, struct fuse_file_info *info) { (void)path; (void)info; return 0; }
static int rufs_flush(const char *path, struct fuse_file_info *info) { (void)path; (void)info; return dev_sync(); }
static int rufs_utimens(const char *path, const struct timespec times[2]) {
    struct inode inode;
    int error = get_node_by_path(path, 0, &inode);
    if (error < 0) return error;
    time_t now = time(NULL);
    if (!times || times[0].tv_nsec == UTIME_NOW) inode.vstat.st_atime = now;
    else if (times[0].tv_nsec != UTIME_OMIT) inode.vstat.st_atime = times[0].tv_sec;
    if (!times || times[1].tv_nsec == UTIME_NOW) inode.vstat.st_mtime = now;
    else if (times[1].tv_nsec != UTIME_OMIT) inode.vstat.st_mtime = times[1].tv_sec;
    inode.vstat.st_ctime = now;
    return writei(inode.ino, &inode);
}

static struct fuse_operations rufs_ope = {
    .init = rufs_init, .destroy = rufs_destroy, .getattr = rufs_getattr,
    .readdir = rufs_readdir, .opendir = rufs_opendir, .mkdir = rufs_mkdir,
    .create = rufs_create, .open = rufs_open, .read = rufs_read, .write = rufs_write,
    .rmdir = rufs_rmdir, .unlink = rufs_unlink, .truncate = rufs_truncate,
    .releasedir = rufs_releasedir, .release = rufs_release, .flush = rufs_flush,
    .utimens = rufs_utimens
};

#ifndef RUFS_NO_MAIN
int main(int argc, char **argv) {
    if (!getcwd(diskfile_path, sizeof(diskfile_path))) { perror("getcwd"); return EXIT_FAILURE; }
    size_t length = strlen(diskfile_path);
    if (length + sizeof("/DISKFILE") > sizeof(diskfile_path)) {
        fputs("Working directory path is too long\n", stderr);
        return EXIT_FAILURE;
    }
    memcpy(diskfile_path + length, "/DISKFILE", sizeof("/DISKFILE"));
    /* All callbacks run serially, including when the caller omits -s. */
    char **arguments = calloc((size_t)argc + 2, sizeof(*arguments));
    if (!arguments) return EXIT_FAILURE;
    for (int i = 0; i < argc; i++) arguments[i] = argv[i];
    arguments[argc] = "-s";
    int result = fuse_main(argc + 1, arguments, &rufs_ope, NULL);
    free(arguments);
    return result;
}
#endif
