/* Copyright (C) 2023 CS416 Rutgers CS. */
#include "block.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

static int diskfile = -1;
_Static_assert(DISK_SIZE % BLOCK_SIZE == 0, "Disk must contain whole blocks");

int dev_init(const char *path) {
    if (diskfile >= 0) return -EBUSY;
    int fd = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) return -errno;
    if (ftruncate(fd, DISK_SIZE) < 0) {
        int error = -errno;
        close(fd);
        unlink(path);
        return error;
    }
    diskfile = fd;
    return 0;
}

int dev_open(const char *path) {
    if (diskfile >= 0) return -EBUSY;
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) return -errno;
    struct stat status;
    if (fstat(fd, &status) < 0) {
        int error = -errno;
        close(fd);
        return error;
    }
    if (!S_ISREG(status.st_mode) || status.st_size != DISK_SIZE) {
        close(fd);
        return -EINVAL;
    }
    diskfile = fd;
    return 0;
}

void dev_close(void) {
    if (diskfile >= 0) close(diskfile);
    diskfile = -1;
}

int dev_sync(void) {
    if (diskfile < 0) return -EBADF;
    return fsync(diskfile) < 0 ? -errno : 0;
}

int bio_read(int block_num, void *buffer) {
    if (diskfile < 0) return -EBADF;
    if (!buffer || block_num < 0 || block_num >= DISK_BLOCKS) return -EINVAL;
    size_t done = 0;
    while (done < BLOCK_SIZE) {
        ssize_t count = pread(diskfile, (char *)buffer + done, BLOCK_SIZE - done,
                              (off_t)block_num * BLOCK_SIZE + (off_t)done);
        if (count < 0) {
            if (errno == EINTR) continue;
            return -errno;
        }
        if (!count) return -EIO;
        done += (size_t)count;
    }
    return BLOCK_SIZE;
}

int bio_write(int block_num, const void *buffer) {
    if (diskfile < 0) return -EBADF;
    if (!buffer || block_num < 0 || block_num >= DISK_BLOCKS) return -EINVAL;
    size_t done = 0;
    while (done < BLOCK_SIZE) {
        ssize_t count = pwrite(diskfile, (const char *)buffer + done, BLOCK_SIZE - done,
                               (off_t)block_num * BLOCK_SIZE + (off_t)done);
        if (count < 0) {
            if (errno == EINTR) continue;
            return -errno;
        }
        if (!count) return -EIO;
        done += (size_t)count;
    }
    return BLOCK_SIZE;
}
