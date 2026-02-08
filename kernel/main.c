#include <arch/arch.h>
#include <base/attributes.h>
#include <sys/vfs.h>

NORETURN void kernel_main(void* boot_info) {
    arch_init(boot_info);
    vfs_init();

    for (;;)
        ;
}
