#pragma once

#include <arch/context.h>
#include <base/types.h>
#include <lib/boot.h>
#include <stddef.h>
#include <sys/types.h>

typedef struct arch_vm_space arch_vm_space_t;
typedef void (*arch_syscall_handler_t)(arch_int_state_t *state);

const kernel_args_t *arch_init(void *boot_info);
void arch_storage_init(void);
void arch_register_devices(void);

#define PHYS_MAP_DEFAULT 0
#define PHYS_MAP_WC      (1U << 0) // write-combining

void *arch_phys_map(u64 paddr, size_t size, u32 flags);
void arch_phys_unmap(void *vaddr, size_t size);
bool arch_phys_copy(u64 dst_paddr, u64 src_paddr, size_t size);

void arch_dump_stack_trace(void);

arch_vm_space_t *arch_vm_kernel(void);
arch_vm_space_t *arch_vm_create_user(void);
void arch_vm_destroy(arch_vm_space_t *space);
void arch_vm_switch(arch_vm_space_t *space);
void *arch_vm_root(arch_vm_space_t *space);

void arch_tlb_flush(uintptr_t addr);

void arch_cpu_set_local(void *ptr);

unsigned long arch_irq_save(void);
void arch_irq_restore(unsigned long flags);

void arch_cpu_halt(void);
void arch_cpu_wait(void);

void arch_irq_disable(void);

u64 arch_timer_ticks(void);
u32 arch_timer_hz(void);
u64 arch_wallclock_seconds(void);

const char *arch_name(void);
const char *arch_cpu_name(void);

u64 arch_cpu_khz(void);

size_t arch_mem_total(void);
size_t arch_mem_free(void);

void arch_syscall_install(int vector, arch_syscall_handler_t handler);

void arch_set_kernel_stack(uintptr_t sp);
void arch_panic_enter(void);

void arch_fpu_init(void *buf);
void arch_fpu_save(void *buf);
void arch_fpu_restore(const void *buf);
