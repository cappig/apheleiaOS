#include <arch/arch.h>
#include <base/attributes.h>

NORETURN void kernel_main(void* boot_info) {
    arch_init(boot_info);

    for (;;)
        ;
}
