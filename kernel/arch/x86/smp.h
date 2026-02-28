#pragma once

#include <base/types.h>
#include <x86/boot.h>

#define SMP_IPI_TLB_VECTOR 0xF0

void smp_set_boot_info(const boot_info_t *info);
void smp_init(void);
void smp_tlb_shootdown(uintptr_t addr);
size_t smp_online_count(void);
