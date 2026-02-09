#pragma once

#include <base/types.h>
#include <stddef.h>
#include <sys/font.h>
#include <sys/types.h>

typedef struct arch_vm_space arch_vm_space_t;

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
bool arch_console_get_size(size_t* cols, size_t* rows);
ssize_t arch_tty_read(void* buf, size_t len);
ssize_t arch_tty_write(const void* buf, size_t len);

arch_vm_space_t* arch_vm_kernel(void);
arch_vm_space_t* arch_vm_create_user(void);
void arch_vm_destroy(arch_vm_space_t* space);
void arch_vm_switch(arch_vm_space_t* space);
void* arch_vm_root(arch_vm_space_t* space);
