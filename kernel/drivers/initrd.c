#include "initrd.h"

#include <base/addr.h>
#include <boot/proto.h>
#include <fs/ustar.h>
#include <log/log.h>

static void* initrd_vaddr;
static usize initrd_size;


ustar_file* initrd_find(const char* name) {
    return ustar_find(initrd_vaddr, initrd_size, name);
}

void initrd_init(boot_handoff* handoff) {
    initrd_vaddr = (void*)ID_MAPPED_VADDR(handoff->initrd_loc);
    initrd_size = handoff->initrd_size;
}

void initrd_close(boot_handoff* handoff) {
    void* paddr = (void*)ID_MAPPED_PADDR(initrd_vaddr);

    if (mmap_free_inner(&handoff->mmap, paddr))
        log_error("Failed to free initrd.tar");
}
