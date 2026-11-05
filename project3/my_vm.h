/* CS416 Project 3 — Aarya Kansara and Aarya Dalal */
#ifndef MY_VM_H_INCLUDED
#define MY_VM_H_INCLUDED
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

#define VA_BITS 32u
#ifndef PGSIZE
#define PGSIZE 4096u
#endif
#ifndef MAX_MEMSIZE
#define MAX_MEMSIZE (1ULL << 32)
#endif
#ifndef MEMSIZE
#define MEMSIZE (1ULL << 30)
#endif
#ifndef TLB_ENTRIES
#define TLB_ENTRIES 512u
#endif
_Static_assert(PGSIZE >= 4096u && PGSIZE <= (1u << 30) &&
               (PGSIZE & (PGSIZE - 1)) == 0, "Page size must be a power of two from 4 KB to 1 GB");
_Static_assert(MAX_MEMSIZE <= (1ULL << 32) && MAX_MEMSIZE > PGSIZE &&
               MAX_MEMSIZE % PGSIZE == 0, "Invalid virtual memory size");
_Static_assert(MEMSIZE >= PGSIZE && MEMSIZE <= (1ULL << 32) &&
               MEMSIZE % PGSIZE == 0 && MEMSIZE <= SIZE_MAX, "Invalid physical memory size");
_Static_assert(TLB_ENTRIES > 0, "TLB must have at least one entry");

#define PTXSHIFT ((unsigned)__builtin_ctz((unsigned)PGSIZE))
#define PT_BITS ((VA_BITS - PTXSHIFT + 1u) / 2u)
#define PD_BITS (VA_BITS - PTXSHIFT - PT_BITS)
#define PDXSHIFT (PTXSHIFT + PT_BITS)
#define PT_ENTRIES (1u << PT_BITS)
#define PD_ENTRIES (1u << PD_BITS)
#define PXMASK (PT_ENTRIES - 1u)
#define OFFMASK (PGSIZE - 1u)
#define PFN_SHIFT PTXSHIFT
#define PDX(va) ((uint32_t)(uintptr_t)(va) >> PDXSHIFT)
#define PTX(va) (((uint32_t)(uintptr_t)(va) >> PTXSHIFT) & PXMASK)
#define OFF(va) ((uint32_t)(uintptr_t)(va) & OFFMASK)

typedef uint32_t vaddr32_t;
typedef uint32_t paddr32_t;
typedef uint32_t pte_t;
typedef uint32_t pde_t;
static inline vaddr32_t VA2U(void *va) { return (vaddr32_t)(uintptr_t)va; }
static inline void *U2VA(vaddr32_t address) { return (void *)(uintptr_t)address; }

struct tlb {
    uint32_t vpn;
    uint32_t pfn;
    bool valid;
};
extern struct tlb tlb_store[TLB_ENTRIES];

void set_physical_mem(void);
int TLB_add(void *va, void *pa);
pte_t *TLB_check(void *va);
void print_TLB_missrate(void);
/* Translation results are thread-local snapshots, valid until the next lookup
 * in that thread. pgdir must be the single root at physical_mem. */
pte_t *translate(pde_t *pgdir, void *va);
int map_page(pde_t *pgdir, void *va, void *pa);
void *get_next_avail(int num_pages);
void *n_malloc(unsigned int num_bytes);
void n_free(void *va, int size);
int put_data(void *va, void *val, int size);
void get_data(void *va, void *val, int size);
void mat_mult(void *mat1, void *mat2, int size, void *answer);
#endif
