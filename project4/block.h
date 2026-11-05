/* Copyright (C) 2023 CS416 Rutgers CS. */
#ifndef RUFS_BLOCK_H
#define RUFS_BLOCK_H
#define BLOCK_SIZE 4096
#ifndef DISK_SIZE
#define DISK_SIZE (32 * 1024 * 1024)
#endif
#define DISK_BLOCKS (DISK_SIZE / BLOCK_SIZE)
int dev_init(const char *path);
int dev_open(const char *path);
void dev_close(void);
int dev_sync(void);
/* Full blocks return BLOCK_SIZE; failures return a negative errno. */
int bio_read(int block_num, void *buffer);
int bio_write(int block_num, const void *buffer);
#endif
