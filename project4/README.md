# Project 4 — RUFS

A small disk-backed filesystem using FUSE 2, implemented for CS416 by Aarya Dalal and Aarya Kansara. Files and directories use sixteen direct block pointers. Metadata, bitmaps, directory entries, and file contents live in one disk image.

## Build and mount

On Ubuntu, install `libfuse-dev`, `fuse`, `pkg-config`, GCC or Clang, and Make.

```sh
make
mkdir /tmp/rufs-mount
./rufs -s /tmp/rufs-mount
ls -la /tmp/rufs-mount
mkdir /tmp/rufs-mount/example
printf 'hello\n' > /tmp/rufs-mount/example/file
cat /tmp/rufs-mount/example/file
fusermount -u /tmp/rufs-mount
```

Run the daemon from the same working directory to reopen its `DISKFILE`. Add `-f` to stay in the foreground. The daemon always uses single-threaded FUSE dispatch. Creating a new file works with ordinary shell redirection. Truncating an existing file, deleting files, and deleting directories return an unsupported-operation error.

The disk image is 32 MB, blocks are 4 KB, and regular files support up to 64 KB. Filenames support up to 207 bytes. The data-block limit is calculated from the space remaining after the metadata region. Sparse regions read as zeros. Zero-byte writes preserve file size. Directory enumeration supports repeated requests with offset cookies.

## Tests

```sh
make test
python3 tests/mount.py
```

Callback tests cover formatting, persistence, directory limits, sparse and unaligned I/O, negative offsets, duplicate creation, disk exhaustion, injected I/O failures, and invalid images. Without libfuse, `make test` uses test-only FUSE declarations and can run on macOS. The mounted test requires Linux FUSE and an accessible `/dev/fuse` device.

GitHub Actions builds with GCC and Clang against actual FUSE headers, runs address and undefined-behavior sanitizers, and checks file operations through a real mount and remount.

## Storage behavior

Superblocks use full zero-padded block buffers. Inodes are packed into slots that never cross block boundaries. Allocation failures release newly reserved resources. Writes prepare their changes before committing and attempt to restore old data and metadata on I/O failure. The filesystem has no journal, so power loss or repeated failures during rollback can leave an inconsistent image.

Existing images are opened without reformatting. Invalid geometry and truncated images are rejected. The on-disk inode retains the coursework's native `struct stat`, so images must be reopened with the same ABI and architecture. Images from the original code with an invalid data-block count are rejected.
