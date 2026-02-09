#pragma once

#include <base/types.h>
#include <stddef.h>
#include <sys/font.h>
#include <sys/types.h>

void arch_init(void* boot_info);
void arch_storage_init(void);

void* arch_phys_map(u64 paddr, size_t size);
void arch_phys_unmap(void* vaddr, size_t size);

u32 arch_pci_read(u8 bus, u8 slot, u8 func, u8 offset, u8 size);

void arch_dump_stack_trace(void);

const char* arch_font_path(void);
void arch_set_font(const font_t* font);

ssize_t arch_console_read(void* buf, size_t len);
ssize_t arch_console_write(const void* buf, size_t len);
ssize_t arch_console_write_screen(size_t screen, const void* buf, size_t len);
bool arch_console_set_active(size_t screen);
ssize_t arch_tty_read(void* buf, size_t len);
ssize_t arch_tty_write(const void* buf, size_t len);
