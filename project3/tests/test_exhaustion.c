#include "../my_vm.h"
#include <assert.h>
#include <errno.h>
extern unsigned char *phys_bitmap, *virt_bitmap;
extern void *physical_mem;
int main(void) {
    /* Only two frames: a directory and data frame leave no space for a table. */
    for (int attempt = 0; attempt < 3; attempt++) {
        assert(!n_malloc(1));
        assert(errno == ENOMEM);
        assert(phys_bitmap[0] == 1);
        assert(virt_bitmap[0] == 1);
        assert(((pde_t *)physical_mem)[0] == 0);
        assert(get_next_avail(1) == U2VA(PGSIZE));
    }
    puts("PASS page-table allocation failure rollback");
    return 0;
}
