#include "smp.h"

#include <arch/arch.h>
#include <arch/thread.h>
#include <base/attributes.h>
#include <base/macros.h>
#include <inttypes.h>
#include <log/log.h>
#include <sched/scheduler.h>
#include <string.h>
#include <sys/cpu.h>
#include <sys/lock.h>
#include <sys/panic.h>
#include <x86/apic.h>
#include <x86/asm.h>
#include <x86/gdt.h>
#include <x86/idt.h>
#include <x86/irq.h>
#include <x86/tsc.h>

#define SMP_AP_STACK_SIZE        (16 * 1024)
#define SMP_AP_START_TIMEOUT_MS  250
#define SMP_TLB_TIMEOUT_MS       5000
#define SMP_TLB_MAX_TARGET_CORES 64
#define SMP_TRAMPOLINE_PAGE_SIZE 0x1000U

static const boot_info_t *smp_boot_info = NULL;
static bool smp_started = false;
static bool smp_shootdown_enabled = false;

ALIGNED(16)
static u8 smp_ap_stacks[MAX_CORES][SMP_AP_STACK_SIZE] = {0};
static volatile u8 smp_ap_ready[MAX_CORES] = {0};

static spinlock_t smp_tlb_lock = SPINLOCK_INIT;
static volatile uintptr_t smp_tlb_addr = 0;
static volatile u64 smp_tlb_targets ALIGNED(8) = 0;
static volatile u64 smp_tlb_acks ALIGNED(8) = 0;

#if defined(__x86_64__)
extern const u8 smp_trampoline64_start;
extern const u8 smp_trampoline64_end;
extern const u8 smp_trampoline64_cr3;
extern const u8 smp_trampoline64_efer;
extern const u8 smp_trampoline64_entry;
extern const u8 smp_trampoline64_arg;
extern const u8 smp_trampoline64_stack;
#else
extern const u8 smp_trampoline32_start;
extern const u8 smp_trampoline32_end;
extern const u8 smp_trampoline32_cr3;
extern const u8 smp_trampoline32_entry;
extern const u8 smp_trampoline32_arg;
extern const u8 smp_trampoline32_stack;
#endif
static uintptr_t _read_stack_ptr(void) {
    uintptr_t sp = 0;
#if defined(__x86_64__)
    asm volatile("mov %%rsp, %0" : "=r"(sp));
#else
    asm volatile("mov %%esp, %0" : "=r"(sp));
#endif
    return sp;
}

static inline u64 _tsc_timeout_deadline(size_t timeout_ms) {
    u64 khz = tsc_khz();
    if (!khz) {
        return 0;
    }

    return read_tsc() + khz * (u64)timeout_ms;
}

static inline bool _tsc_timed_out(u64 deadline) {
    if (!deadline) {
        return false;
    }

    return read_tsc() >= deadline;
}

#ifdef MMU_DEBUG
static void _log_tlb_timeout_state(u64 targets, u64 acks) {
    for (size_t i = 0; i < core_count && i < SMP_TLB_MAX_TARGET_CORES; i++) {
        if (!(targets & (1ULL << i))) {
            continue;
        }

        sched_thread_t *thread = sched_current_core(i);
        const char *name = thread ? thread->name : "<none>";
        pid_t pid = thread ? thread->pid : -1;

        log_warn(
            "tlb wait core=%zu acked=%u current=%s pid=%d",
            i,
            (acks & (1ULL << i)) ? 1 : 0,
            name,
            pid
        );
    }
}
#endif

static void _fpu_enable_local(void) {
    u64 cr0 = read_cr0();
    cr0 &= ~(u64)(CR0_EM | CR0_TS);
    cr0 |= (u64)CR0_MP;
    write_cr0(cr0);

    u64 cr4 = read_cr4();
    cr4 |= (u64)(CR4_OSFXSR | CR4_OSXMMEXCPT);
    write_cr4(cr4);

    asm volatile("fninit");
}

NORETURN static void _smp_ap_entry(void *arg) {
    size_t expected = (size_t)(uintptr_t)arg;
    disable_interrupts();

    if (!apic_init()) {
        cpu_halt();
    }

    cpu_core_t *core = cpu_find_by_lapic(lapic_id());

    if (!core || core->id >= MAX_CORES) {
        cpu_halt();
    }

    cpuid_regs_t regs = {0};
    cpuid(1, &regs);
    u32 cpuid_apic_id = (regs.ebx >> 24) & 0xffU;

    if (cpuid_apic_id != core->lapic_id) {
        log_warn(
            "AP core %zu APIC ID mismatch (cpuid=%u lapic=%u)",
            core->id,
            cpuid_apic_id,
            core->lapic_id
        );
    }

    if (expected < MAX_CORES && core->id != expected) {
        log_warn(
            "AP core mismatch (expected=%zu, actual=%zu)",
            expected,
            core->id
        );
    }

    cpu_set_current(core);
    _fpu_enable_local();

    gdt_init();
    tss_init(_read_stack_ptr());
    idt_load();
    irq_init_ap();
    scheduler_init_core();
    cpu_set_online(core, true);

    __atomic_store_n(&smp_ap_ready[core->id], 1, __ATOMIC_RELEASE);

    while (!sched_is_running()) {
        arch_cpu_relax();
    }

    if (smp_online_count() > 1) {
        smp_shootdown_enabled = true;
    }

    scheduler_start_secondary();
    enable_interrupts();
    cpu_halt();
}

static void _write32(void *base, size_t off, u32 value) {
    memcpy((u8 *)base + off, &value, sizeof(value));
}

#if defined(__x86_64__)
static void _write64(void *base, size_t off, u64 value) {
    memcpy((u8 *)base + off, &value, sizeof(value));
}
#endif

static const u8 *_trampoline_start(void) {
#if defined(__x86_64__)
    return &smp_trampoline64_start;
#else
    return &smp_trampoline32_start;
#endif
}

static const u8 *_trampoline_end(void) {
#if defined(__x86_64__)
    return &smp_trampoline64_end;
#else
    return &smp_trampoline32_end;
#endif
}

static size_t _trampoline_size(void) {
    return (size_t)(_trampoline_end() - _trampoline_start());
}

static void _patch_trampoline(void *trampoline_base, const cpu_core_t *core) {
    u64 stack_top =
        (u64)(uintptr_t)(smp_ap_stacks[core->id] + ARRAY_LEN(smp_ap_stacks[0]));

#if defined(__x86_64__)
    size_t off_cr3 = (size_t)(&smp_trampoline64_cr3 - &smp_trampoline64_start);
    size_t off_efer =
        (size_t)(&smp_trampoline64_efer - &smp_trampoline64_start);
    size_t off_entry =
        (size_t)(&smp_trampoline64_entry - &smp_trampoline64_start);
    size_t off_arg = (size_t)(&smp_trampoline64_arg - &smp_trampoline64_start);
    size_t off_stack =
        (size_t)(&smp_trampoline64_stack - &smp_trampoline64_start);

    _write32(trampoline_base, off_cr3, (u32)read_cr3());
    _write32(trampoline_base, off_efer, (u32)read_msr(EFER_MSR));
    _write64(
        trampoline_base,
        off_entry,
        (u64)(uintptr_t)_smp_ap_entry
    );
    _write64(trampoline_base, off_arg, (u64)core->id);
    _write64(trampoline_base, off_stack, stack_top);
#else
    size_t off_cr3 = (size_t)(&smp_trampoline32_cr3 - &smp_trampoline32_start);
    size_t off_entry =
        (size_t)(&smp_trampoline32_entry - &smp_trampoline32_start);
    size_t off_arg = (size_t)(&smp_trampoline32_arg - &smp_trampoline32_start);
    size_t off_stack =
        (size_t)(&smp_trampoline32_stack - &smp_trampoline32_start);

    _write32(trampoline_base, off_cr3, (u32)read_cr3());
    _write32(
        trampoline_base,
        off_entry,
        (u32)(uintptr_t)_smp_ap_entry
    );
    _write32(trampoline_base, off_arg, (u32)core->id);
    _write32(trampoline_base, off_stack, (u32)stack_top);
#endif
}

static bool _wait_ap_ready(size_t core_id, size_t timeout_ms) {
    if (core_id >= MAX_CORES) {
        return false;
    }

    u64 deadline = _tsc_timeout_deadline(timeout_ms);
    size_t fallback = 4000000 * timeout_ms;

    while (__atomic_load_n(&smp_ap_ready[core_id], __ATOMIC_ACQUIRE) == 0) {
        if (deadline && _tsc_timed_out(deadline)) {
            return false;
        }

        if (!deadline && !fallback--) {
            return false;
        }

        arch_cpu_relax();
    }

    return true;
}

static bool _start_ap(const cpu_core_t *core, u8 vector) {
    if (!core) {
        return false;
    }

    bool init_ok = lapic_send_init(core->lapic_id);
    tsc_spin(10);

    bool sipi1_ok = lapic_send_startup(core->lapic_id, vector);
    tsc_spin(1);
    bool sipi2_ok = lapic_send_startup(core->lapic_id, vector);

    return init_ok && (sipi1_ok || sipi2_ok);
}

static void _tlb_ipi_handler(UNUSED int_state_t *state) {
    uintptr_t addr = __atomic_load_n(&smp_tlb_addr, __ATOMIC_ACQUIRE);

    if (addr) {
#if defined(__x86_64__)
        tlb_flush((u64)addr);
#else
        tlb_flush((u32)addr);
#endif
    }

    cpu_core_t *core = cpu_current();
    if (core && core->id < SMP_TLB_MAX_TARGET_CORES) {
        __atomic_or_fetch(
            &smp_tlb_acks, 1ULL << core->id, __ATOMIC_SEQ_CST
        );
    }

    lapic_end_int();
}

static void _resched_ipi_handler(UNUSED int_state_t *state) {
    lapic_end_int();
    sched_resched_softirq((arch_int_state_t *)state);
}

void smp_set_boot_info(const boot_info_t *info) {
    smp_boot_info = info;
}

size_t smp_online_count(void) {
    return __atomic_load_n(&core_online_count, __ATOMIC_ACQUIRE);
}

void smp_init(void) {
    if (smp_started) {
        return;
    }

    smp_started = true;
    set_int_handler(SMP_IPI_TLB_VECTOR, _tlb_ipi_handler);
    set_int_handler(SMP_IPI_RESCHED_VECTOR, _resched_ipi_handler);

    if (core_count <= 1) {
        log_info("online cores: 1/1");
        return;
    }

    if (!smp_boot_info || !smp_boot_info->smp_trampoline_paddr) {
        log_warn("trampoline page unavailable, staying uniprocessor");
        return;
    }

    u64 trampoline_paddr = smp_boot_info->smp_trampoline_paddr;

    if (
        (trampoline_paddr & (SMP_TRAMPOLINE_PAGE_SIZE - 1)) ||
        trampoline_paddr >= 0x100000ULL
    ) {
        log_warn("invalid trampoline address %#" PRIx64, trampoline_paddr);
        return;
    }

    u8 sipi_vector = (u8)(trampoline_paddr >> 12);
    void *trampoline = 
        arch_phys_map(trampoline_paddr, SMP_TRAMPOLINE_PAGE_SIZE, PHYS_MAP_DEFAULT);

    if (!trampoline) {
        log_warn("failed to map trampoline page");
        return;
    }

    size_t trampoline_size = _trampoline_size();

    if (!trampoline_size || trampoline_size > SMP_TRAMPOLINE_PAGE_SIZE) {
        arch_phys_unmap(trampoline, SMP_TRAMPOLINE_PAGE_SIZE);
        log_warn("invalid trampoline blob size");
        return;
    }

    memset(trampoline, 0, SMP_TRAMPOLINE_PAGE_SIZE);
    memcpy(trampoline, _trampoline_start(), trampoline_size);
    size_t started = 0;

    for (size_t i = 1; i < core_count && i < MAX_CORES; i++) {
        cpu_core_t *core = &cores_local[i];

        if (!core->valid) {
            continue;
        }

        __atomic_store_n(&smp_ap_ready[i], 0, __ATOMIC_RELEASE);
        _patch_trampoline(trampoline, core);

        if (!_start_ap(core, sipi_vector)) {
            log_warn("AP start IPI failed for core %zu (lapic=%u)", i, core->lapic_id);
            continue;
        }

        if (!_wait_ap_ready(i, SMP_AP_START_TIMEOUT_MS)) {
            log_warn("AP bring-up timed out for core %zu (lapic=%u)", i, core->lapic_id);
            continue;
        }

        started++;
    }

    arch_phys_unmap(trampoline, SMP_TRAMPOLINE_PAGE_SIZE);

    size_t online = smp_online_count();
    if (online > 1) {
        smp_shootdown_enabled = true;
    }

    size_t prepared = started + 1;
    if (prepared > core_count) {
        prepared = core_count;
    }

    log_info("online cores: %zu/%zu", online, core_count);
}

bool smp_send_resched(size_t core_id) {
    if (core_id >= MAX_CORES) {
        return false;
    }

    cpu_core_t *target = &cores_local[core_id];
    cpu_core_t *self = cpu_current();

    if (!target->valid || !target->online) {
        return false;
    }

    if (self && self->id == core_id) {
        return false;
    }

    return lapic_send_fixed(target->lapic_id, SMP_IPI_RESCHED_VECTOR);
}

void smp_tlb_shootdown(uintptr_t addr) {
    if (!sched_is_running()) {
        return;
    }

    if (!smp_shootdown_enabled || smp_online_count() <= 1) {
        return;
    }

    cpu_core_t *self = cpu_current();
    if (!self || self->id >= SMP_TLB_MAX_TARGET_CORES) {
        return;
    }

    arch_word_t user_top = arch_user_stack_top();
    bool user_addr = user_top && addr < (uintptr_t)user_top;
    sched_thread_t *self_thread = sched_current();
    arch_vm_space_t *self_vm = self_thread ? self_thread->vm_space : NULL;

    unsigned long irq_flags = arch_irq_save();

    spin_lock(&smp_tlb_lock);

    u64 targets = 0;

    for (size_t i = 0; i < core_count && i < SMP_TLB_MAX_TARGET_CORES; i++) {
        cpu_core_t *core = &cores_local[i];

        if (!core->valid || !core->online || i == self->id) {
            continue;
        }

        if (user_addr) {
            sched_thread_t *remote = sched_current_core(i);
            arch_vm_space_t *remote_vm = remote ? remote->vm_space : NULL;

            if (!self_vm || remote_vm != self_vm) {
                continue;
            }
        }

        targets |= 1ULL << i;
    }

    if (!targets) {
        spin_unlock(&smp_tlb_lock);
        arch_irq_restore(irq_flags);
        return;
    }

    __atomic_store_n(&smp_tlb_addr, addr, __ATOMIC_RELEASE);
    __atomic_store_n(&smp_tlb_targets, targets, __ATOMIC_RELEASE);
    __atomic_store_n(&smp_tlb_acks, 0, __ATOMIC_RELEASE);

    for (size_t i = 0; i < core_count && i < SMP_TLB_MAX_TARGET_CORES; i++) {
        if (!(targets & (1ULL << i))) {
            continue;
        }

        cpu_core_t *core = &cores_local[i];
        if (!lapic_send_fixed(core->lapic_id, SMP_IPI_TLB_VECTOR)) {
            panic("failed to send TLB shootdown IPI");
        }
    }

    u64 deadline = _tsc_timeout_deadline(SMP_TLB_TIMEOUT_MS);
    size_t fallback = (size_t)(4000000ULL * SMP_TLB_TIMEOUT_MS);

    while (
        (__atomic_load_n(&smp_tlb_acks, __ATOMIC_ACQUIRE) &
         __atomic_load_n(&smp_tlb_targets, __ATOMIC_ACQUIRE)) != targets
    ) {
        if (deadline && _tsc_timed_out(deadline)) {
            u64 acks = __atomic_load_n(&smp_tlb_acks, __ATOMIC_ACQUIRE);
            u64 pending =
                __atomic_load_n(&smp_tlb_targets, __ATOMIC_ACQUIRE);

#ifdef MMU_DEBUG
            _log_tlb_timeout_state(pending, acks);
#endif

            panic(
                "TLB shootdown timeout (self=%zu targets=%#" PRIx64
                " acks=%#" PRIx64 " online=%zu)",
                self->id,
                pending,
                acks,
                smp_online_count()
            );
        }

        if (!deadline && !fallback--) {
            u64 acks = __atomic_load_n(&smp_tlb_acks, __ATOMIC_ACQUIRE);
            u64 pending =
                __atomic_load_n(&smp_tlb_targets, __ATOMIC_ACQUIRE);

#ifdef MMU_DEBUG
            _log_tlb_timeout_state(pending, acks);
#endif

            panic(
                "TLB shootdown timeout (self=%zu targets=%#" PRIx64
                " acks=%#" PRIx64 " online=%zu)",
                self->id,
                pending,
                acks,
                smp_online_count()
            );
        }

        arch_cpu_relax();
    }

    __atomic_store_n(&smp_tlb_acks, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&smp_tlb_targets, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&smp_tlb_addr, 0, __ATOMIC_RELEASE);

    spin_unlock(&smp_tlb_lock);
    arch_irq_restore(irq_flags);
}
