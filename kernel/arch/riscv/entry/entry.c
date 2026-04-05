#include <kernel.h>
#include <riscv/asm.h>
#include <riscv/boot.h>
#include <riscv/serial.h>

NORETURN void _kern_entry(boot_info_t *info) {
    uintptr_t uart_base = SERIAL_UART0;
    if (info && info->uart_paddr) {
        uart_base = (uintptr_t)info->uart_paddr;
    }

    send_serial_string(uart_base, "riscv: _kern_entry\n\r");
    kernel_main(info);
    halt();
}
