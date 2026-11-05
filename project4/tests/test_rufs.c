#include "../block.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
static int checked_read(int block, void *buffer);
static int checked_write(int block, const void *buffer);
#define bio_read checked_read
#define bio_write checked_write
#define RUFS_NO_MAIN
#include "../rufs.c"
#undef bio_read
#undef bio_write

static int fail_read_block = -1;
static int fail_write_block = -1;

/* Full-block copies let sanitizers inspect both sides of the I/O boundary. */
static int checked_read(int block, void *buffer) {
    if (block == fail_read_block) { fail_read_block = -1; return -EIO; }
    unsigned char full[BLOCK_SIZE];
    int result = bio_read(block, full);
    if (result >= 0) memcpy(buffer, full, sizeof(full));
    return result;
}
static int checked_write(int block, const void *buffer) {
    if (block == fail_write_block) { fail_write_block = -1; return -EIO; }
    unsigned char full[BLOCK_SIZE];
    memcpy(full, buffer, sizeof(full));
    return bio_write(block, full);
}

static void fresh(void) {
    dev_close();
    assert(unlink(diskfile_path) == 0 || errno == ENOENT);
    assert(rufs_mkfs() == 0);
}
static int used_bits(int block, unsigned count) {
    union disk_block buffer;
    assert(bio_read(block, buffer.bytes) == BLOCK_SIZE);
    int used = 0;
    for (unsigned i = 0; i < count; i++) used += get_bitmap(buffer.bytes, (int)i);
    return used;
}
static int inode_count(void) { return used_bits((int)sb.i_bitmap_blk, sb.max_inum); }
static int block_count(void) { return used_bits((int)sb.d_bitmap_blk, sb.max_dnum); }

static void metadata_and_persistence(void) {
    fresh();
    union disk_block super;
    assert(bio_read(0, super.bytes) == BLOCK_SIZE);
    for (size_t i = sizeof(sb); i < BLOCK_SIZE; i++) assert(super.bytes[i] == 0);
    assert(sb.d_start_blk + sb.max_dnum <= DISK_BLOCKS);
    struct stat status;
    assert(rufs_getattr("/", &status) == 0);
    assert(S_ISDIR(status.st_mode) && status.st_nlink == 2 && status.st_mtime > 0);
    assert(rufs_mkdir("/dir", 0750) == 0);
    assert(rufs_create("/dir/file", 0640, NULL) == 0);
    assert(rufs_write("/dir/file", "persisted", 9, 0, NULL) == 9);
    assert(rufs_getattr("/", &status) == 0 && status.st_nlink == 3);
    assert(rufs_getattr("/dir/file", &status) == 0 && status.st_size == 9);
    assert((status.st_mode & 0777) == 0640);
    assert(rufs_flush("/dir/file", NULL) == 0);
    dev_close();
    memset(&sb, 0, sizeof(sb));
    assert(load_filesystem() == 0);
    char value[9];
    assert(rufs_read("/dir/file", value, sizeof(value), 0, NULL) == 9);
    assert(!memcmp(value, "persisted", 9));
    assert(stat(diskfile_path, &status) == 0 && status.st_size == DISK_SIZE);
    puts("PASS metadata and remount persistence");
}

static void invalid_operations(void) {
    fresh();
    assert(rufs_create("/file", 0644, NULL) == 0);
    int count = inode_count();
    for (int i = 0; i < 10; i++) {
        assert(rufs_create("/file", 0644, NULL) == -EEXIST);
        assert(rufs_mkdir("/file", 0755) == -EEXIST);
    }
    assert(inode_count() == count);
    assert(rufs_create("/file/child", 0644, NULL) == -ENOTDIR);
    assert(rufs_opendir("/file", NULL) == -ENOTDIR);
    assert(rufs_open("/", NULL) == -EISDIR);
    assert(rufs_read("/", NULL, 0, 0, NULL) == -EISDIR);
    assert(rufs_write("/", "x", 1, 0, NULL) == -EISDIR);
    assert(rufs_write("/file", "x", 1, -1, NULL) == -EINVAL);
    assert(rufs_read("/file", NULL, 0, -1, NULL) == -EINVAL);
    assert(rufs_write("/file", NULL, 0, 100, NULL) == 0);
    struct stat status;
    assert(rufs_getattr("/file", &status) == 0 && status.st_size == 0);
    char path[NAME_CAPACITY + 2];
    path[0] = '/';
    memset(path + 1, 'a', NAME_CAPACITY);
    path[NAME_CAPACITY + 1] = '\0';
    assert(rufs_create(path, 0644, NULL) == -ENAMETOOLONG);
    path[NAME_CAPACITY] = '\0';
    assert(rufs_create(path, 0644, NULL) == 0);
    assert(rufs_getattr(path, &status) == 0);
    assert(rufs_unlink("/file") == -EOPNOTSUPP);
    assert(rufs_truncate("/file", 0) == -EOPNOTSUPP);
    puts("PASS invalid operations and inode rollback");
}

static void data_and_sparse_files(void) {
    fresh();
    assert(rufs_create("/file", 0644, NULL) == 0);
    unsigned char *input = malloc(MAX_FILE_SIZE), *output = malloc(MAX_FILE_SIZE);
    assert(input && output);
    for (size_t i = 0; i < MAX_FILE_SIZE; i++) input[i] = (unsigned char)(i * 37);
    assert(rufs_write("/file", (char *)input, MAX_FILE_SIZE, 0, NULL) == MAX_FILE_SIZE);
    assert(rufs_read("/file", (char *)output, MAX_FILE_SIZE, 0, NULL) == MAX_FILE_SIZE);
    assert(!memcmp(input, output, MAX_FILE_SIZE));
    assert(rufs_write("/file", "unaligned", 9, BLOCK_SIZE - 3, NULL) == 9);
    assert(rufs_read("/file", (char *)output, 9, BLOCK_SIZE - 3, NULL) == 9);
    assert(!memcmp(output, "unaligned", 9));
    assert(rufs_write("/file", "x", 1, MAX_FILE_SIZE, NULL) == -EFBIG);
    assert(rufs_read("/file", (char *)output, 1, MAX_FILE_SIZE, NULL) == 0);
    assert(rufs_create("/sparse", 0644, NULL) == 0);
    assert(rufs_write("/sparse", "X", 1, 2 * BLOCK_SIZE + 7, NULL) == 1);
    assert(rufs_read("/sparse", (char *)output, MAX_FILE_SIZE, 0, NULL) == 2 * BLOCK_SIZE + 8);
    for (size_t i = 0; i < 2 * BLOCK_SIZE + 7; i++) assert(output[i] == 0);
    assert(output[2 * BLOCK_SIZE + 7] == 'X');
    assert(rufs_write("/sparse", "Y", 1, 2 * BLOCK_SIZE + 20, NULL) == 1);
    assert(rufs_read("/sparse", (char *)output, 14, 2 * BLOCK_SIZE + 7, NULL) == 14);
    assert(output[0] == 'X' && output[13] == 'Y');
    for (int i = 1; i < 13; i++) assert(output[i] == 0);
    free(input); free(output);
    puts("PASS full-size, unaligned and sparse I/O");
}

struct listing { size_t count; off_t next; size_t capacity; char names[512][NAME_CAPACITY]; };
static int collect(void *argument, const char *name, const struct stat *status, off_t next) {
    (void)status;
    struct listing *list = argument;
    if (list->count == list->capacity) return 1;
    assert(list->count < 512);
    snprintf(list->names[list->count++], NAME_CAPACITY, "%s", name);
    list->next = next;
    return 0;
}

static void directories(void) {
    fresh();
    assert(rufs_mkdir("/full", 0755) == 0);
    unsigned capacity = DIRECT_COUNT * DIRENTS_PER_BLOCK - 2;
    for (unsigned i = 0; i < capacity; i++) {
        char path[64]; snprintf(path, sizeof(path), "/full/f%03u", i);
        assert(rufs_create(path, 0644, NULL) == 0);
    }
    int inodes = inode_count(), blocks = block_count();
    assert(rufs_create("/full/overflow", 0644, NULL) == -ENOSPC);
    assert(rufs_mkdir("/full/overflow", 0755) == -ENOSPC);
    assert(inode_count() == inodes && block_count() == blocks);
    struct listing list = {.capacity = 2};
    off_t offset = 0;
    while (list.count < capacity + 2) {
        size_t before = list.count;
        assert(rufs_readdir("/full", &list, collect, offset, NULL) == 0);
        assert(list.count > before);
        offset = list.next;
        list.capacity += 2;
    }
    assert(list.count == capacity + 2);
    assert(!strcmp(list.names[0], ".") && !strcmp(list.names[1], ".."));
    for (unsigned i = 0; i < capacity; i++) {
        char name[32]; snprintf(name, sizeof(name), "f%03u", i);
        assert(!strcmp(list.names[i + 2], name));
    }
    assert(rufs_readdir("/full/f000", &list, collect, 0, NULL) == -ENOTDIR);
    puts("PASS directory capacity and paginated readdir");
}

static void disk_full(void) {
    fresh();
    assert(rufs_create("/file", 0644, NULL) == 0);
    union disk_block before, after;
    memset(&before, 0xff, sizeof(before));
    unset_bitmap(before.bytes, sb.max_dnum - 1);
    unset_bitmap(before.bytes, sb.max_dnum - 2);
    assert(bio_write((int)sb.d_bitmap_blk, before.bytes) == BLOCK_SIZE);
    char data[3 * BLOCK_SIZE] = {0};
    assert(rufs_write("/file", data, sizeof(data), 0, NULL) == -ENOSPC);
    assert(bio_read((int)sb.d_bitmap_blk, after.bytes) == BLOCK_SIZE);
    assert(!memcmp(&before, &after, sizeof(before)));
    struct stat status;
    assert(rufs_getattr("/file", &status) == 0 && status.st_size == 0);
    assert(rufs_mkdir("/dir", 0755) == 0);
    puts("PASS disk-full write rollback");
}

static void io_failures(void) {
    fresh();
    int count = inode_count();
    fail_write_block = (int)sb.d_start_blk;
    assert(rufs_create("/failed", 0644, NULL) == -EIO);
    assert(inode_count() == count);
    struct stat status;
    assert(rufs_getattr("/failed", &status) == -ENOENT);
    assert(rufs_create("/file", 0644, NULL) == 0);
    assert(rufs_write("/file", "old", 3, 0, NULL) == 3);
    struct inode file;
    assert(get_node_by_path("/file", 0, &file) == 0);
    fail_read_block = file.direct_ptr[0];
    char output[3];
    assert(rufs_read("/file", output, 3, 0, NULL) == -EIO);
    fail_write_block = file.direct_ptr[0];
    assert(rufs_write("/file", "new", 3, 0, NULL) == -EIO);
    assert(rufs_read("/file", output, 3, 0, NULL) == 3 && !memcmp(output, "old", 3));
    int blocks = block_count();
    fail_write_block = (int)(sb.i_start_blk + file.ino / INODES_PER_BLOCK);
    char data[2 * BLOCK_SIZE] = {0};
    assert(rufs_write("/file", data, sizeof(data), 0, NULL) == -EIO);
    assert(block_count() == blocks);
    assert(rufs_getattr("/file", &status) == 0 && status.st_size == 3);
    assert(rufs_read("/file", output, 3, 0, NULL) == 3 && !memcmp(output, "old", 3));
    puts("PASS injected I/O failures and rollback");
}

static void invalid_images_and_block_layer(void) {
    fresh();
    union disk_block buffer;
    assert(bio_read(-1, buffer.bytes) == -EINVAL);
    assert(bio_write(DISK_BLOCKS, buffer.bytes) == -EINVAL);
    assert(dev_open(diskfile_path) == -EBUSY);
    dev_close();
    assert(bio_read(0, buffer.bytes) == -EBADF);
    assert(dev_open(diskfile_path) == 0);
    assert(bio_read(0, buffer.bytes) == BLOCK_SIZE);
    union disk_block corrupt = buffer;
    corrupt.super.max_dnum = MAX_DNUM;
    assert(bio_write(0, corrupt.bytes) == BLOCK_SIZE);
    dev_close();
    assert(load_filesystem() == -EINVAL);
    assert(bio_read(0, buffer.bytes) == BLOCK_SIZE && !memcmp(&buffer, &corrupt, sizeof(buffer)));
    dev_close();
    fresh();
    int fd = open(diskfile_path, O_RDWR);
    assert(fd >= 0 && ftruncate(fd, DISK_SIZE - 1) == 0);
    close(fd);
    assert(bio_read(DISK_BLOCKS - 1, buffer.bytes) == -EIO);
    dev_close();
    assert(dev_open(diskfile_path) == -EINVAL);
    puts("PASS disk bounds, short reads and invalid-image handling");
}

int main(void) {
    (void)&rufs_ope;
    char folder[] = "/tmp/rufs-tests-XXXXXX";
    assert(mkdtemp(folder));
    assert(snprintf(diskfile_path, sizeof(diskfile_path), "%s/DISKFILE", folder) > 0);
    metadata_and_persistence();
    invalid_operations();
    data_and_sparse_files();
    directories();
    disk_full();
    io_failures();
    invalid_images_and_block_layer();
    dev_close();
    assert(unlink(diskfile_path) == 0);
    assert(rmdir(folder) == 0);
    return 0;
}
