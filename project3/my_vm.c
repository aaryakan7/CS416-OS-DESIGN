/* CS416 Project 3 — Aarya Kansara and Aarya Dalal */
#include "my_vm.h"
#include <errno.h>
#include <limits.h>
#include <string.h>

void *physical_mem;
unsigned char *phys_bitmap;
unsigned char *virt_bitmap;
unsigned long num_phys_pages;
unsigned long num_virt_pages;
struct tlb tlb_store[TLB_ENTRIES];
double tlb_lookups;
double tlb_misses;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
int is_initialized;

static size_t table_users[PD_ENTRIES];
static _Thread_local pte_t translation_result;

static void set_bit(unsigned char *bitmap, size_t index) {
    bitmap[index / 8] |= (unsigned char)(1u << (index % 8));
}
static void clear_bit(unsigned char *bitmap, size_t index) {
    bitmap[index / 8] &= (unsigned char)~(1u << (index % 8));
}
static int get_bit(const unsigned char *bitmap, size_t index) {
    return (bitmap[index / 8] >> (index % 8)) & 1u;
}

/* All helpers ending in _locked require the global mutex. */
static int init_locked(void) {
    if (is_initialized) return 0;
    num_phys_pages = MEMSIZE / PGSIZE;
    num_virt_pages = MAX_MEMSIZE / PGSIZE;
    physical_mem = calloc(1, MEMSIZE);
    phys_bitmap = calloc((num_phys_pages + 7) / 8, 1);
    virt_bitmap = calloc((num_virt_pages + 7) / 8, 1);
    if (!physical_mem || !phys_bitmap || !virt_bitmap) {
        free(physical_mem);
        free(phys_bitmap);
        free(virt_bitmap);
        physical_mem = NULL;
        phys_bitmap = virt_bitmap = NULL;
        errno = ENOMEM;
        return -1;
    }
    /* Frame zero contains the directory; virtual page zero is never allocated. */
    set_bit(phys_bitmap, 0);
    set_bit(virt_bitmap, 0);
    memset(tlb_store, 0, sizeof(tlb_store));
    is_initialized = 1;
    return 0;
}

void set_physical_mem(void) {
    pthread_mutex_lock(&mutex);
    init_locked();
    pthread_mutex_unlock(&mutex);
}

static int address_valid(void *va) {
    uint64_t address = (uintptr_t)va;
    return address >= PGSIZE && address < MAX_MEMSIZE;
}

static long free_frame_locked(void) {
    for (unsigned long frame = 1; frame < num_phys_pages; frame++) {
        if (!get_bit(phys_bitmap, frame)) return (long)frame;
    }
    return -1;
}

static pte_t *entry_locked(vaddr32_t va) {
    pde_t *directory = physical_mem;
    pde_t table_frame = directory[PDX(va)];
    if (!table_frame) return NULL;
    pte_t *table = (pte_t *)((char *)physical_mem + (size_t)table_frame * PGSIZE);
    return &table[PTX(va)];
}

static void tlb_add_locked(vaddr32_t va, pte_t frame) {
    uint32_t vpn = va / PGSIZE;
    tlb_store[vpn % TLB_ENTRIES] = (struct tlb){vpn, frame, true};
}

static struct tlb *tlb_check_locked(vaddr32_t va) {
    uint32_t vpn = va / PGSIZE;
    struct tlb *entry = &tlb_store[vpn % TLB_ENTRIES];
    tlb_lookups++;
    if (entry->valid && entry->vpn == vpn) return entry;
    tlb_misses++;
    return NULL;
}

static pte_t frame_locked(vaddr32_t va) {
    struct tlb *cached = tlb_check_locked(va);
    if (cached) return cached->pfn;
    pte_t *entry = entry_locked(va);
    if (!entry || !*entry) return 0;
    tlb_add_locked(va, *entry);
    return *entry;
}

int TLB_add(void *va, void *pa) {
    pthread_mutex_lock(&mutex);
    int result = -1;
    if (is_initialized && address_valid(va) && (uint64_t)(uintptr_t)pa < MEMSIZE) {
        pte_t *entry = entry_locked(VA2U(va));
        pte_t frame = (pte_t)((uintptr_t)pa / PGSIZE);
        if (entry && *entry && *entry == frame) {
            tlb_add_locked(VA2U(va), frame);
            result = 0;
        }
    }
    if (result) errno = EINVAL;
    pthread_mutex_unlock(&mutex);
    return result;
}

pte_t *TLB_check(void *va) {
    pthread_mutex_lock(&mutex);
    pte_t *result = NULL;
    if (is_initialized && address_valid(va)) {
        struct tlb *cached = tlb_check_locked(VA2U(va));
        if (cached) {
            translation_result = cached->pfn;
            result = &translation_result;
        }
    }
    pthread_mutex_unlock(&mutex);
    return result;
}

void print_TLB_missrate(void) {
    pthread_mutex_lock(&mutex);
    double lookups = tlb_lookups;
    double misses = tlb_misses;
    pthread_mutex_unlock(&mutex);
    fprintf(stderr, "TLB lookups: %.0lf, TLB misses: %.0lf\n", lookups, misses);
    fprintf(stderr, "TLB miss rate %lf \n", lookups ? misses / lookups : 0.0);
}

pte_t *translate(pde_t *pgdir, void *va) {
    pthread_mutex_lock(&mutex);
    pte_t *result = NULL;
    if (is_initialized && pgdir == physical_mem && address_valid(va)) {
        pte_t frame = frame_locked(VA2U(va));
        if (frame) {
            translation_result = frame;
            result = &translation_result;
        }
    }
    if (!result) errno = EFAULT;
    pthread_mutex_unlock(&mutex);
    return result;
}

static int map_locked(vaddr32_t va, pte_t frame) {
    uint32_t vpn = va / PGSIZE;
    pte_t *existing = entry_locked(va);
    if (existing && *existing) {
        if (*existing == frame) return 0;
        errno = EEXIST;
        return -1;
    }
    if (!frame || frame >= num_phys_pages || get_bit(phys_bitmap, frame)) {
        errno = EINVAL;
        return -1;
    }
    /* Reserve the data frame before searching for a page-table frame. */
    set_bit(phys_bitmap, frame);
    pde_t *directory = physical_mem;
    unsigned index = PDX(va);
    if (!directory[index]) {
        long table_frame = free_frame_locked();
        if (table_frame < 0) {
            clear_bit(phys_bitmap, frame);
            errno = ENOMEM;
            return -1;
        }
        set_bit(phys_bitmap, (size_t)table_frame);
        memset((char *)physical_mem + (size_t)table_frame * PGSIZE, 0, PGSIZE);
        directory[index] = (pde_t)table_frame;
    }
    *entry_locked(va) = frame;
    table_users[index]++;
    set_bit(virt_bitmap, vpn);
    memset((char *)physical_mem + (size_t)frame * PGSIZE, 0, PGSIZE);
    /* Installing a new mapping is one compulsory translation miss. */
    tlb_lookups++;
    tlb_misses++;
    tlb_add_locked(va, frame);
    return 0;
}

int map_page(pde_t *pgdir, void *va, void *pa) {
    pthread_mutex_lock(&mutex);
    int result = -1;
    if (!is_initialized || pgdir != physical_mem || !address_valid(va) ||
        OFF(va) || (uintptr_t)pa % PGSIZE || (uint64_t)(uintptr_t)pa >= MEMSIZE) {
        errno = EINVAL;
    } else {
        result = map_locked(VA2U(va), (pte_t)((uintptr_t)pa / PGSIZE));
    }
    pthread_mutex_unlock(&mutex);
    return result;
}

static void *next_avail_locked(size_t pages) {
    if (!pages || pages >= num_virt_pages) return NULL;
    size_t count = 0;
    for (size_t vpn = 1; vpn < num_virt_pages; vpn++) {
        if (get_bit(virt_bitmap, vpn)) count = 0;
        else if (++count == pages) return U2VA((vaddr32_t)((vpn + 1 - pages) * PGSIZE));
    }
    return NULL;
}

void *get_next_avail(int num_pages) {
    pthread_mutex_lock(&mutex);
    void *result = NULL;
    if (num_pages > 0 && init_locked() == 0) result = next_avail_locked((size_t)num_pages);
    pthread_mutex_unlock(&mutex);
    return result;
}

static void unmap_locked(vaddr32_t va) {
    pte_t *entry = entry_locked(va);
    clear_bit(phys_bitmap, *entry);
    *entry = 0;
    uint32_t vpn = va / PGSIZE;
    clear_bit(virt_bitmap, vpn);
    struct tlb *cached = &tlb_store[vpn % TLB_ENTRIES];
    if (cached->valid && cached->vpn == vpn) cached->valid = false;
    unsigned index = PDX(va);
    if (--table_users[index] == 0) {
        pde_t *directory = physical_mem;
        clear_bit(phys_bitmap, directory[index]);
        directory[index] = 0;
    }
}

void *n_malloc(unsigned int num_bytes) {
    if (!num_bytes) { errno = EINVAL; return NULL; }
    pthread_mutex_lock(&mutex);
    if (init_locked() != 0) {
        pthread_mutex_unlock(&mutex);
        return NULL;
    }
    size_t pages = ((uint64_t)num_bytes + PGSIZE - 1) / PGSIZE;
    void *base = next_avail_locked(pages);
    if (!base) {
        errno = ENOMEM;
        pthread_mutex_unlock(&mutex);
        return NULL;
    }
    double previous_lookups = tlb_lookups;
    double previous_misses = tlb_misses;
    size_t mapped = 0;
    for (; mapped < pages; mapped++) {
        long frame = free_frame_locked();
        if (frame < 0 || map_locked(VA2U(base) + (vaddr32_t)(mapped * PGSIZE), (pte_t)frame)) break;
    }
    if (mapped != pages) {
        for (size_t i = 0; i < mapped; i++) unmap_locked(VA2U(base) + (vaddr32_t)(i * PGSIZE));
        tlb_lookups = previous_lookups;
        tlb_misses = previous_misses;
        base = NULL;
        errno = ENOMEM;
    }
    pthread_mutex_unlock(&mutex);
    return base;
}

/* Validate before changing any page or caller buffer. Wide arithmetic prevents
 * ranges near 4 GB from wrapping into virtual page zero. */
static int range_valid_locked(void *va, uint64_t bytes) {
    uint64_t start = (uintptr_t)va;
    if (!is_initialized || !address_valid(va) || !bytes || bytes > MAX_MEMSIZE - start) return 0;
    uint64_t last = (start + bytes - 1) / PGSIZE;
    for (uint64_t vpn = start / PGSIZE; vpn <= last; vpn++) {
        pte_t *entry = entry_locked((vaddr32_t)(vpn * PGSIZE));
        if (!entry || !*entry) return 0;
    }
    return 1;
}

void n_free(void *va, int size) {
    pthread_mutex_lock(&mutex);
    if (size <= 0 || OFF(va) || !range_valid_locked(va, (uint64_t)size)) {
        errno = EINVAL;
    } else {
        size_t pages = ((uint64_t)size + PGSIZE - 1) / PGSIZE;
        for (size_t i = 0; i < pages; i++) unmap_locked(VA2U(va) + (vaddr32_t)(i * PGSIZE));
    }
    pthread_mutex_unlock(&mutex);
}

static void copy_locked(vaddr32_t address, void *buffer, size_t bytes, bool write_data) {
    char *data = buffer;
    while (bytes) {
        size_t offset = OFF(address);
        size_t chunk = PGSIZE - offset;
        if (chunk > bytes) chunk = bytes;
        pte_t frame = frame_locked(address);
        char *physical = (char *)physical_mem + (size_t)frame * PGSIZE + offset;
        if (write_data) memcpy(physical, data, chunk);
        else memcpy(data, physical, chunk);
        address += (vaddr32_t)chunk;
        data += chunk;
        bytes -= chunk;
    }
}

int put_data(void *va, void *val, int size) {
    pthread_mutex_lock(&mutex);
    int result = -1;
    if (val && size > 0 && range_valid_locked(va, (uint64_t)size)) {
        copy_locked(VA2U(va), val, (size_t)size, true);
        result = 0;
    } else errno = EFAULT;
    pthread_mutex_unlock(&mutex);
    return result;
}

void get_data(void *va, void *val, int size) {
    pthread_mutex_lock(&mutex);
    if (val && size > 0 && range_valid_locked(va, (uint64_t)size)) {
        copy_locked(VA2U(va), val, (size_t)size, false);
    } else errno = EFAULT;
    pthread_mutex_unlock(&mutex);
}

void mat_mult(void *mat1, void *mat2, int size, void *answer) {
    pthread_mutex_lock(&mutex);
    uint64_t bytes = size > 0 ? (uint64_t)size * (unsigned)size * sizeof(int) : 0;
    if (!bytes || bytes > SIZE_MAX || !range_valid_locked(mat1, bytes) ||
        !range_valid_locked(mat2, bytes) || !range_valid_locked(answer, bytes)) {
        errno = EFAULT;
        pthread_mutex_unlock(&mutex);
        return;
    }
    int *result = malloc((size_t)bytes);
    if (!result) {
        errno = ENOMEM;
        pthread_mutex_unlock(&mutex);
        return;
    }
    for (int i = 0; i < size; i++) {
        for (int j = 0; j < size; j++) {
            int64_t sum = 0;
            for (int k = 0; k < size; k++) {
                int left, right;
                copy_locked(VA2U(mat1) + (vaddr32_t)(((size_t)i * size + k) * sizeof(int)), &left, sizeof(left), false);
                copy_locked(VA2U(mat2) + (vaddr32_t)(((size_t)k * size + j) * sizeof(int)), &right, sizeof(right), false);
                if (__builtin_add_overflow(sum, (int64_t)left * right, &sum)) goto overflow;
            }
            if (sum < INT_MIN || sum > INT_MAX) goto overflow;
            result[(size_t)i * size + j] = (int)sum;
        }
    }
    copy_locked(VA2U(answer), result, (size_t)bytes, true);
    free(result);
    pthread_mutex_unlock(&mutex);
    return;
overflow:
    errno = ERANGE;
    free(result);
    pthread_mutex_unlock(&mutex);
}
