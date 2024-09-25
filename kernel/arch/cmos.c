#include "cmos.h"

#include <base/types.h>
#include <stdlib.h>
#include <x86/asm.h>


static u8 read_rtc_reg(u16 reg) {
    outb(CMOS_ADDR, reg);

    return bcdtoi(inb(CMOS_DATA));
}

std_time get_time() {
    std_time time = {0};

    // Check if the CMOS is updating and break when it's not
    while (inb(CMOS_DATA) & 0x80)
        outb(CMOS_ADDR, 0x0a);

    time.tm_year = read_rtc_reg(0x09) + 100;
    time.tm_mon = read_rtc_reg(0x08) - 1;
    time.tm_mday = read_rtc_reg(0x07);
    time.tm_wday = read_rtc_reg(0x06) - 1;

    time.tm_hour = read_rtc_reg(0x04);
    time.tm_min = read_rtc_reg(0x02);
    time.tm_sec = read_rtc_reg(0x00);

    return time;
}
