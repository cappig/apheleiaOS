#include <base/attributes.h>
#include <riscv/asm.h>

#include "tty.h"

NORETURN void boot_main(void) {
    printf("starting apheleiaOS\n\r");

    halt();
}
