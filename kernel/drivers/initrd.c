#include "initrd.h"

#include <base/addr.h>
#include <boot/proto.h>
#include <fs/ustar.h>
#include <stddef.h>

#include "arch/panic.h"

static void* initrd_vaddr;
static usize initrd_size;


void* initrd_find(const char* name) {
    void* header = ustar_find(initrd_vaddr, initrd_size, name);
    if (!header)
        return NULL;

    return header + USTAR_BLOCK_SIZE;
}

void initrd_init(boot_handoff* handoff) {
    initrd_vaddr = (void*)ID_MAPPED_VADDR(handoff->initrd_loc);
    initrd_size = handoff->initrd_size;
}

void initrd_close(boot_handoff* handoff) {
    void* paddr = (void*)ID_MAPPED_PADDR(initrd_vaddr);

    if (mmap_free_inner(&handoff->mmap, paddr))
        panic("Failed to free initrd.tar");
}