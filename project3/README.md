# Project 3 — Virtual memory

A simulated 32-bit address space backed by a buffer of physical memory, two-level page tables, bitmaps, and a direct-mapped TLB. Originally developed by Aarya Kansara and Aarya Dalal for CS416.

## Build and test

Requires GCC or Clang, Make, and pthreads. Builds on Linux and macOS with the host's native address width.

```sh
make
make test
make benchmarks
./benchmark/test
./benchmark/mtest
```

For a 32-bit Linux build, install the multilib development libraries and add `-m32` to `CFLAGS`. GitHub Actions checks GCC and Clang on Linux at both 32 and 64 bits, plus memory and thread sanitizers.

The default simulated memory sizes are 1 GB physical and 4 GB virtual, with 4 KB pages and 512 TLB entries. Configuration macros can be supplied through `CPPFLAGS`, for example `make CPPFLAGS='-DPGSIZE=8192 -DTLB_ENTRIES=7'`. The library and benchmarks must use the same configuration. `make test` independently tests 4 KB, 8 KB, and 64 KB pages using smaller physical memory buffers and different TLB capacities.

## Behavior

Page tables live inside simulated physical memory and are created on demand. Empty page tables are reclaimed when their last mapping is removed. Each allocation receives whole pages, and failed allocations release all pages and page tables acquired during that attempt.

`put_data` returns zero on success and -1 on failure. Invalid calls set `errno`. The void APIs `get_data`, `n_free`, and `mat_mult` report failure through `errno` and preserve the destination or mappings on failure. Read and write ranges must be fully mapped. Freeing requires a page-aligned address and validates the entire rounded page range before releasing it. Newly allocated data pages are zeroed.

All public operations synchronize through one mutex. Matrix multiplication holds that mutex for the full operation, supports output aliasing an input, and rejects results that overflow `int`. Applications must still coordinate the lifetime of allocations across separate API calls.

Virtual addresses are opaque simulated addresses. Use `U2VA` and `VA2U` for arithmetic and the data APIs for access. The helpers convert values; the public APIs validate addresses before converting them. Translation functions return thread-local frame-number snapshots, valid until the next lookup in that thread. The single page-directory argument must point to the root at `physical_mem`. `map_page` takes a simulated physical byte address, reserves an unused frame, and rejects conflicting mappings. `get_next_avail` searches without reserving pages.

The simulated memory buffer and bitmaps remain allocated for the lifetime of the process. Four-level 64-bit paging and sub-page allocation are outside this implementation.
