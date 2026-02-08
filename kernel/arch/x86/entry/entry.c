#include <base/attributes.h>
#include <x86/asm.h>
#include <x86/boot.h>

NORETURN void kernel_main(void* boot_info);

NORETURN void _kern_entry(boot_info_t* info) {
    kernel_main(info);

    halt();
}
