#include "../my_vm.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <string.h>

extern void *physical_mem;
extern unsigned char *phys_bitmap, *virt_bitmap;
extern unsigned long num_phys_pages, num_virt_pages;

static size_t used_pages(const unsigned char *bitmap, unsigned long pages) {
    size_t used = 0;
    for (unsigned long i = 0; i < pages; i++) used += (bitmap[i / 8] >> (i % 8)) & 1u;
    return used;
}
static void assert_empty(void) {
    assert(used_pages(phys_bitmap, num_phys_pages) == 1);
    assert(used_pages(virt_bitmap, num_virt_pages) == 1);
    for (size_t i = 0; i < PD_ENTRIES; i++) assert(((pde_t *)physical_mem)[i] == 0);
    for (size_t i = 0; i < TLB_ENTRIES; i++) assert(!tlb_store[i].valid);
}

static void boundaries(void) {
    void *va = n_malloc(PGSIZE);
    assert(va);
    unsigned char *data = malloc(PGSIZE + 1);
    assert(data);
    memset(data, 0x42, PGSIZE + 1);
    assert(put_data(va, data, PGSIZE + 1) == -1);
    unsigned char first = 99;
    get_data(va, &first, 1);
    assert(first == 0);
    memset(data, 0x37, PGSIZE + 1);
    get_data(va, data, PGSIZE + 1);
    for (size_t i = 0; i < PGSIZE + 1; i++) assert(data[i] == 0x37);
    errno = 0;
    n_free(va, 2 * PGSIZE);
    assert(errno == EINVAL);
    assert(translate(physical_mem, va));
    errno = 0;
    n_free(U2VA(VA2U(va) + 1), 1);
    assert(errno == EINVAL);
    assert(put_data(U2VA(UINT32_MAX - 1), data, 4) == -1);
#if UINTPTR_MAX > UINT32_MAX
    assert(put_data((void *)(uintptr_t)((1ULL << 32) + VA2U(va)), data, 1) == -1);
#endif
    assert(put_data(va, data, 1) == 0);
    n_free(va, PGSIZE);
    assert(!translate(physical_mem, va));
    assert(!TLB_check(va));
    void *again = n_malloc(1);
    assert(again == va);
    get_data(again, &first, 1);
    assert(first == 0);
    n_free(again, 1);
    free(data);
    assert_empty();
}

static void crossing_and_tlb(void) {
    unsigned bytes = (PT_ENTRIES + 2) * PGSIZE;
    void *va = n_malloc(bytes);
    assert(va);
    unsigned char source[32], output[32];
    for (size_t i = 0; i < sizeof(source); i++) source[i] = (unsigned char)i;
    void *cross = U2VA(VA2U(va) + PT_ENTRIES * PGSIZE - 9);
    assert(put_data(cross, source, sizeof(source)) == 0);
    get_data(cross, output, sizeof(output));
    assert(!memcmp(source, output, sizeof(source)));
    /* Force a cache miss and verify the page-table walk repopulates the TLB. */
    void *collision = U2VA(VA2U(va) + TLB_ENTRIES * PGSIZE);
    if (TLB_ENTRIES < PT_ENTRIES + 2) {
        assert(translate(physical_mem, va));
        assert(translate(physical_mem, collision));
        assert(!TLB_check(va));
        assert(translate(physical_mem, va));
        assert(TLB_check(va));
    }
    n_free(va, (int)bytes);
    assert_empty();
}

static void allocation_failure(void) {
    assert(!n_malloc(0));
    assert(!n_malloc(UINT_MAX));
    assert(!n_malloc((unsigned)MEMSIZE));
    assert_empty();
    void *va = n_malloc(1);
    assert(va);
    n_free(va, 1);
    assert_empty();
}

static void manual_mapping(void) {
    void *va = U2VA((vaddr32_t)(MAX_MEMSIZE - PGSIZE));
    assert(map_page(NULL, va, U2VA(PGSIZE)) == -1);
    assert(map_page(physical_mem, va, U2VA(PGSIZE)) == 0);
    assert(*translate(physical_mem, va) == 1);
    assert(TLB_add(va, U2VA(3 * PGSIZE)) == -1);
    assert(map_page(physical_mem, va, U2VA(3 * PGSIZE)) == -1);
    assert(map_page(physical_mem, U2VA(PGSIZE), U2VA(PGSIZE)) == -1);
    int value = 416, result = 0;
    void *end = U2VA((vaddr32_t)(MAX_MEMSIZE - sizeof(value)));
    assert(put_data(end, &value, sizeof(value)) == 0);
    get_data(end, &result, sizeof(result));
    assert(result == value);
    assert(put_data(end, &value, sizeof(value) + 1) == -1);
    n_free(va, PGSIZE);
    assert_empty();
}

static void matrices(void) {
    int a[] = {1, 2, 3, 4}, b[] = {5, 6, 7, 8};
    int expected[] = {19, 22, 43, 50}, output[4];
    void *va = n_malloc(sizeof(a)), *vb = n_malloc(sizeof(b));
    assert(va && vb);
    assert(put_data(va, a, sizeof(a)) == 0);
    assert(put_data(vb, b, sizeof(b)) == 0);
    mat_mult(va, vb, 2, va); /* Output may alias an input. */
    get_data(va, output, sizeof(output));
    assert(!memcmp(output, expected, sizeof(output)));
    int huge = INT_MAX, two = 2;
    assert(put_data(va, &huge, sizeof(huge)) == 0);
    assert(put_data(vb, &two, sizeof(two)) == 0);
    errno = 0;
    mat_mult(va, vb, 1, va);
    assert(errno == ERANGE);
    get_data(va, output, sizeof(huge));
    assert(output[0] == INT_MAX);
    n_free(va, sizeof(a));
    n_free(vb, sizeof(b));
    assert_empty();
}

static void *concurrent(void *argument) {
    intptr_t id = (intptr_t)argument;
    set_physical_mem();
    for (int iteration = 0; iteration < 20; iteration++) {
        void *va = n_malloc(PGSIZE + 8);
        assert(va);
        int value = (int)id * 100 + iteration, result = 0;
        void *cross = U2VA(VA2U(va) + PGSIZE - 2);
        assert(put_data(cross, &value, sizeof(value)) == 0);
        get_data(cross, &result, sizeof(result));
        assert(value == result);
        pte_t *frame = translate(physical_mem, cross);
        assert(frame && *frame);
        n_free(va, PGSIZE + 8);
    }
    return NULL;
}

int main(void) {
    /* Exercise initialization from multiple threads before any other API call. */
    pthread_t threads[50];
    for (intptr_t i = 0; i < 50; i++) assert(!pthread_create(&threads[i], NULL, concurrent, (void *)i));
    for (int i = 0; i < 50; i++) assert(!pthread_join(threads[i], NULL));
    assert_empty();
    boundaries();
    crossing_and_tlb();
    allocation_failure();
    manual_mapping();
    matrices();
    printf("PASS page size %u, TLB entries %u\n", (unsigned)PGSIZE, (unsigned)TLB_ENTRIES);
    return 0;
}
