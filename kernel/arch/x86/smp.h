#pragma once

#include <base/types.h>
#include <x86/boot.h>

#define SMP_IPI_TLB_VECTOR 0xF0
#define SMP_IPI_RESCHED_VECTOR 0xF1
#define SCHED_SOFT_RESCHED_VECTOR 0xF2

void smp_set_boot_info(const boot_info_t *info);
void smp_init(void);
void smp_tlb_shootdown(uintptr_t addr);
bool smp_send_resched(size_t core_id);
size_t smp_online_count(void);
