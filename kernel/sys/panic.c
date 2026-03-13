#include "panic.h"

#include <arch/arch.h>

void panic_dump_state(const arch_int_state_t *state) {
    arch_dump_registers(state);
    arch_dump_stack_trace();
}
