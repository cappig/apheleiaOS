#pragma once

#include <base/types.h>
#include <data/ring.h>

#define PS2_RELEASE_OFFSET 0x80
#define PS2_RELEASED(key)  (key + PS2_RELEASE_OFFSET)

#define PS2_EXTENDED 0xE0

#define PS2_MOUSE_SPECIFIER 0xd4

// https://wiki.osdev.org/%228042%22_PS/2_Controller
// https://wiki.osdev.org/PS/2_Keyboard
// https://wiki.osdev.org/PS/2_Mouse

enum ps2_ports {
    PS2_REG_DATA = 0x60,
    PS2_REG_STATUS = 0x64, // read
    PS2_REG_COMMAND = 0x64, // write
};

enum ps2_status_bits {
    PS2_STA_OUT_BUFFER = 1 << 0,
    PS2_STA_IN_BUFFER = 1 << 1,
    PS2_STA_POST = 1 << 2,
    PS2_STA_COMMAND = 1 << 3,
    // PS2_STA_RESERVED1 = 1 << 4,
    // PS2_STA_RESERVED2 = 1 << 5,
    PS2_STA_TIME_OUT_ERROR = 1 << 6,
    PS2_STA_PARITY_ERROR = 1 << 7,
};

enum ps2_control_bits {
    PS2_CON_PORT1_IRQ = 1 << 0,
    PS2_CON_PORT2_IRQ = 1 << 1,
    PS2_CON_POST = 1 << 2,
    // PS2_CON_ZERO0 = 1 << 3,
    PS2_CON_PORT1_CLOCK = 1 << 4,
    PS2_CON_PORT2_CLOCK = 1 << 5,
    PS2_CON_PORT1_TRANSLATE = 1 << 6,
    // PS2_CON_ZERO1 = 1 << 7,
};

enum ps2_commands {
    PS2_COM_READ_CONFIG = 0x20,
    PS2_COM_WRITE_CONFIG = 0x60,

    PS2_COM_DISABLE_PORT2 = 0xa7,
    PS2_COM_ENABLE_PORT2 = 0xa8,
    PS2_COM_TEST_PORT2 = 0xa9,

    PS2_COM_TEST_CONTROLLER = 0xaa,

    PS2_COM_TEST_PORT1 = 0xab,
    PS2_COM_DISABLE_PORT1 = 0xad,
    PS2_COM_ENABLE_PORT1 = 0xae,
};

enum ps2_kbd_commands {
    PS2_KBD_COM_SET_LED = 0xed,
    PS2_KBD_COM_ECHO = 0xee,
    PS2_KBD_COM_SET_CODESET = 0xf0,
    PS2_KBD_COM_IDENTIFY = 0xf2,
    PS2_KBD_COM_SET_RATE = 0xf3,
    PS2_KBD_COM_RESET = 0xff,
};

enum ps2_mouse_commands {
    PS2_MOUSE_COM_SCALING_ONE = 0xe6, // 1:1 scaling
    PS2_MOUSE_COM_SCALING_TWO = 0x78, // 2:1 scaling
    PS2_MOUSE_COM_SET_SAMPLE = 0xf3,
    PS2_MOUSE_COM_ENABLE_DATA = 0xf4,
    PS2_MOUSE_COM_DISABLE_DATA = 0xf5,
    PS2_MOUSE_COM_DEFAULT = 0xf6,
    PS2_MOUSE_COM_RESET = 0xff,
};

enum ps2_mouse_flags {
    PS2_MOUSE_LEFT = 1 << 0,
    PS2_MOUSE_RIGHT = 1 << 1,
    PS2_MOUSE_MIDDLE = 1 << 2,
    // PS2_MOUSE_IGNORED = 1 << 3,
    PS2_MOUSE_XSIGN = 1 << 4,
    PS2_MOUSE_YSIGN = 1 << 5,
    PS2_MOUSE_XOVERFLOW = 1 << 6,
    PS2_MOUSE_YOVERFLOW = 1 << 7,
};


void init_ps2(void);
