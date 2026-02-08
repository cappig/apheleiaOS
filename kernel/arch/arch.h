#pragma once

#include <base/types.h>
#include <stddef.h>

void arch_init(void* boot_info);

void* arch_phys_map(u64 paddr, size_t size);
void arch_phys_unmap(void* vaddr, size_t size);
