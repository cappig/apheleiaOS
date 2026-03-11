#include "pit.h"

#include <x86/asm.h>

static u32 pit_hz = PIT_DEFAULT_HZ;

static void _program(u32 hz) {
    if (!hz) {
        hz = 1;
    }

    u16 divisor = (u16)(PIT_BASE_FREQ / hz);

    outb(PIT_CONTROL, PIT_SET);
    outb(PIT_A, (u8)(divisor & 0xff));
    outb(PIT_A, (u8)((divisor >> 8) & 0xff));
}

void pit_init(void) {
    pit_set_frequency(PIT_DEFAULT_HZ);
}

void pit_set_frequency(u32 hz) {
    pit_hz = hz ? hz : 1;
    _program(pit_hz);
}

u32 pit_get_frequency(void) {
    return pit_hz;
}
