#include "panic.h"

#include <arch/arch.h>

void panic_trace(void) {
    arch_dump_stack_trace();
}
