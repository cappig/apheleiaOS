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

#define AP_STACK_SIZE        (16 * 1024)
#define AP_START_TIMEOUT_MS  250
#define TLB_RETRY_MS         10
#define TLB_TIMEOUT_MS       5000
#define TLB_MAX_TARGETS      64
#define TRAMPOLINE_PAGE_SIZE 0x1000U

typedef struct {
    const boot_info_t *boot_info;
    bool started;
    bool shootdown_enabled;

    u8 ap_stacks[MAX_CORES][AP_STACK_SIZE] ALIGNED(16);
    volatile u8 ap_ready[MAX_CORES];

    spinlock_t tlb_lock;
    volatile uintptr_t tlb_addr;
    volatile u64 tlb_targets ALIGNED(8);
    volatile u64 tlb_seq ALIGNED(8);
    volatile u64 tlb_seen[TLB_MAX_TARGETS] ALIGNED(8);
} smp_state_t;

static smp_state_t smp = {
    .tlb_lock = SPINLOCK_INIT,
};

#if defined(__x86_64__)
extern const u8 smp_trampoline64_start;
extern const u8 smp_trampoline64_end;
extern const u8 smp_trampoline64_cr0;
extern const u8 smp_trampoline64_cr4;
extern const u8 smp_trampoline64_cr3;
extern const u8 smp_trampoline64_efer;
extern const u8 smp_trampoline64_entry;
extern const u8 smp_trampoline64_arg;
extern const u8 smp_trampoline64_stack;
#else
extern const u8 smp_trampoline32_start;
extern const u8 smp_trampoline32_end;
extern const u8 smp_trampoline32_cr0;
extern const u8 smp_trampoline32_cr4;
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

static inline u64 timeout_deadline(size_t timeout_ms) {
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

static void _write32(void *base, size_t off, u32 value) {
    memcpy((u8 *)base + off, &value, sizeof(value));
}

#if defined(__x86_64__)
static void _write64(void *base, size_t off, u64 value) {
    memcpy((u8 *)base + off, &value, sizeof(value));
}
#endif

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

    cpuid_regs_t regs = { 0 };
    cpuid(1, &regs);
    u32 cpuid_apic_id = (regs.ebx >> 24) & 0xffU;

    if (cpuid_apic_id != core->lapic_id) {
        log_warn(
            "AP core %zu APIC ID mismatch (cpuid=%u lapic=%u)",
            core->id,
            (unsigned int)cpuid_apic_id,
            (unsigned int)core->lapic_id
        );
    }

    if (expected < MAX_CORES && core->id != expected) {
        log_warn("AP core mismatch (expected=%zu, actual=%zu)", expected, core->id);
    }

    cpu_set_current(core);
    _fpu_enable_local();
    pat_init();

    gdt_init();
    tss_init(_read_stack_ptr());
    idt_load();
    irq_init_ap();
    scheduler_init_core();
    cpu_set_online(core, true);

    __atomic_store_n(&smp.ap_ready[core->id], 1, __ATOMIC_RELEASE);

    while (!sched_is_running()) {
        arch_cpu_relax();
    }

    enable_interrupts();
    scheduler_start_cpu();
    cpu_halt();
}

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
    uintptr_t start = (uintptr_t)_trampoline_start();
    uintptr_t end = (uintptr_t)_trampoline_end();

    return end > start ? (size_t)(end - start) : 0;
}

static size_t _trampoline_offset(const u8 *symbol) {
    return (size_t)((uintptr_t)symbol - (uintptr_t)_trampoline_start());
}

static void _patch_trampoline(void *trampoline_base, const cpu_core_t *core) {
    u64 stack_top = (u64)(uintptr_t)(smp.ap_stacks[core->id] + ARRAY_LEN(smp.ap_stacks[0]));

#if defined(__x86_64__)
    size_t off_cr0 = _trampoline_offset(&smp_trampoline64_cr0);
    size_t off_cr4 = _trampoline_offset(&smp_trampoline64_cr4);
    size_t off_cr3 = _trampoline_offset(&smp_trampoline64_cr3);
    size_t off_efer = _trampoline_offset(&smp_trampoline64_efer);
    size_t off_entry = _trampoline_offset(&smp_trampoline64_entry);
    size_t off_arg = _trampoline_offset(&smp_trampoline64_arg);
    size_t off_stack = _trampoline_offset(&smp_trampoline64_stack);

    _write32(trampoline_base, off_cr0, (u32)read_cr0());
    _write32(trampoline_base, off_cr4, (u32)read_cr4());
    _write32(trampoline_base, off_cr3, (u32)read_cr3());
    _write32(trampoline_base, off_efer, (u32)read_msr(EFER_MSR));
    _write64(trampoline_base, off_entry, (u64)(uintptr_t)_smp_ap_entry);
    _write64(trampoline_base, off_arg, (u64)core->id);
    _write64(trampoline_base, off_stack, stack_top);
#else
    size_t off_cr0 = _trampoline_offset(&smp_trampoline32_cr0);
    size_t off_cr4 = _trampoline_offset(&smp_trampoline32_cr4);
    size_t off_cr3 = _trampoline_offset(&smp_trampoline32_cr3);
    size_t off_entry = _trampoline_offset(&smp_trampoline32_entry);
    size_t off_arg = _trampoline_offset(&smp_trampoline32_arg);
    size_t off_stack = _trampoline_offset(&smp_trampoline32_stack);

    _write32(trampoline_base, off_cr0, (u32)read_cr0());
    _write32(trampoline_base, off_cr4, (u32)read_cr4());
    _write32(trampoline_base, off_cr3, (u32)read_cr3());
    _write32(trampoline_base, off_entry, (u32)(uintptr_t)_smp_ap_entry);
    _write32(trampoline_base, off_arg, (u32)core->id);
    _write32(trampoline_base, off_stack, (u32)stack_top);
#endif
}

static bool _wait_ap_ready(size_t core_id, size_t timeout_ms) {
    if (core_id >= MAX_CORES) {
        return false;
    }

    u64 deadline = timeout_deadline(timeout_ms);
    size_t fallback = 4000000 * timeout_ms;

    while (__atomic_load_n(&smp.ap_ready[core_id], __ATOMIC_ACQUIRE) == 0) {
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

static void service_tlb_local(void) {
    cpu_core_t *core = cpu_current();
    if (!core || core->id >= TLB_MAX_TARGETS) {
        return;
    }

    u64 bit = 1ULL << core->id;
    u64 seq = __atomic_load_n(&smp.tlb_seq, __ATOMIC_ACQUIRE);
    u64 seen = __atomic_load_n(&smp.tlb_seen[core->id], __ATOMIC_ACQUIRE);
    if (!seq || seen >= seq) {
        return;
    }

    u64 targets = __atomic_load_n(&smp.tlb_targets, __ATOMIC_ACQUIRE);
    if (!(targets & bit)) {
        return;
    }

    uintptr_t addr = __atomic_load_n(&smp.tlb_addr, __ATOMIC_ACQUIRE);

#if defined(__x86_64__)
    tlb_flush((u64)addr);
#else
    tlb_flush((u32)addr);
#endif

    // Never let a delayed request overwrite a newer acknowledgement.
    while (seen < seq) {
        bool updated = __atomic_compare_exchange_n(
            &smp.tlb_seen[core->id],
            &seen,
            seq,
            false,
            __ATOMIC_RELEASE,
            __ATOMIC_ACQUIRE
        );

        if (updated) {
            break;
        }
    }
}

static void _tlb_ipi_handler(UNUSED int_state_t *state) {
    service_tlb_local();
    lapic_end_int();
}

static void resched_ipi(UNUSED int_state_t *state) {
    lapic_end_int();
    sched_resched_softirq((arch_int_state_t *)state);
}

void smp_set_boot_info(const boot_info_t *info) {
    smp.boot_info = info;
}

size_t smp_online_count(void) {
    return __atomic_load_n(&core_online_count, __ATOMIC_ACQUIRE);
}

void smp_init(void) {
    if (smp.started) {
        return;
    }

    smp.started = true;
    set_int_handler(SMP_IPI_TLB_VECTOR, _tlb_ipi_handler);
    set_int_handler(SMP_IPI_RESCHED_VECTOR, resched_ipi);

    if (core_count <= 1) {
        log_info("online cores: 1/1");
        return;
    }

    if (!smp.boot_info || !smp.boot_info->smp_trampoline_paddr) {
        log_warn("trampoline page unavailable, staying uniprocessor");
        return;
    }

    u64 trampoline_paddr = smp.boot_info->smp_trampoline_paddr;

    if ((trampoline_paddr & (TRAMPOLINE_PAGE_SIZE - 1)) || trampoline_paddr >= 0x100000ULL) {
        log_warn("invalid trampoline address %#" PRIx64, trampoline_paddr);
        return;
    }

    u8 sipi_vector = (u8)(trampoline_paddr >> 12);
    void *trampoline = arch_phys_map(trampoline_paddr, TRAMPOLINE_PAGE_SIZE, PHYS_MAP_DEFAULT);

    if (!trampoline) {
        log_warn("failed to map trampoline page");
        return;
    }

    size_t trampoline_size = _trampoline_size();

    if (!trampoline_size || trampoline_size > TRAMPOLINE_PAGE_SIZE) {
        arch_phys_unmap(trampoline, TRAMPOLINE_PAGE_SIZE);
        log_warn("invalid trampoline blob size");
        return;
    }

    memset(trampoline, 0, TRAMPOLINE_PAGE_SIZE);
    memcpy(trampoline, _trampoline_start(), trampoline_size);
    size_t started = 0;

    for (size_t i = 1; i < core_count && i < MAX_CORES; i++) {
        cpu_core_t *core = &cores_local[i];

        if (!core->valid) {
            continue;
        }

        __atomic_store_n(&smp.ap_ready[i], 0, __ATOMIC_RELEASE);
        _patch_trampoline(trampoline, core);

        if (!_start_ap(core, sipi_vector)) {
            log_warn("AP start IPI failed for core %zu (lapic=%u)", i, (unsigned int)core->lapic_id);
            continue;
        }

        if (!_wait_ap_ready(i, AP_START_TIMEOUT_MS)) {
            log_warn("AP bring-up timed out for core %zu (lapic=%u)", i, (unsigned int)core->lapic_id);
            continue;
        }

        started++;
    }

    arch_phys_unmap(trampoline, TRAMPOLINE_PAGE_SIZE);

    size_t online = smp_online_count();
    if (online > 1) {
        smp.shootdown_enabled = true;
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

static u64 tlb_targets(cpu_core_t *self) {
    u64 targets = 0;

    for (size_t i = 0; i < core_count && i < TLB_MAX_TARGETS; i++) {
        cpu_core_t *core = &cores_local[i];

        if (!core->valid || !core->online || i == self->id) {
            continue;
        }

        targets |= 1ULL << i;
    }

    return targets;
}

static void send_tlb_ipis(u64 targets) {
    for (size_t i = 0; i < core_count && i < TLB_MAX_TARGETS; i++) {
        if (!(targets & (1ULL << i))) {
            continue;
        }

        cpu_core_t *core = &cores_local[i];
        if (!lapic_send_fixed(core->lapic_id, SMP_IPI_TLB_VECTOR)) {
            panic("failed to send TLB shootdown IPI");
        }
    }
}

static u64 tlb_seen_mask(u64 targets, u64 seq) {
    u64 seen = 0;

    for (size_t i = 0; i < core_count && i < TLB_MAX_TARGETS; i++) {
        u64 bit = 1ULL << i;
        if ((targets & bit) && __atomic_load_n(&smp.tlb_seen[i], __ATOMIC_ACQUIRE) >= seq) {
            seen |= bit;
        }
    }

    return seen;
}

static void panic_tlb_timeout(cpu_core_t *self, u64 seq) {
    u64 pending = __atomic_load_n(&smp.tlb_targets, __ATOMIC_ACQUIRE);
    u64 seen = tlb_seen_mask(pending, seq);

    panic(
        "TLB shootdown timeout (self=%zu seq=%" PRIu64 " targets=%#" PRIx64 " seen=%#" PRIx64 " online=%zu)",
        self->id,
        seq,
        pending,
        seen,
        smp_online_count()
    );
}

static void wait_tlb_seen(cpu_core_t *self, u64 targets, u64 seq) {
    u64 deadline = timeout_deadline(TLB_TIMEOUT_MS);
    u64 retry_deadline = timeout_deadline(TLB_RETRY_MS);
    size_t fallback = (size_t)(4000000ULL * TLB_TIMEOUT_MS);
    size_t retry_fallback = (size_t)(4000000ULL * TLB_RETRY_MS);

    while (true) {
        u64 seen = tlb_seen_mask(targets, seq);

        if ((seen & targets) == targets) {
            return;
        }

        bool retry = retry_deadline && _tsc_timed_out(retry_deadline);
        if (!retry_deadline && !retry_fallback--) {
            retry = true;
        }

        if (retry) {
            send_tlb_ipis(targets & ~seen);
            retry_deadline = timeout_deadline(TLB_RETRY_MS);
            retry_fallback = (size_t)(4000000ULL * TLB_RETRY_MS);
        }

        if (deadline && _tsc_timed_out(deadline)) {
            panic_tlb_timeout(self, seq);
        }

        if (!deadline && !fallback--) {
            panic_tlb_timeout(self, seq);
        }

        arch_cpu_relax();
    }
}

static void lock_tlb(void) {
    while (!spin_try_lock(&smp.tlb_lock)) {
        // A caller can arrive with interrupts disabled while another CPU is
        // waiting for its fixed IPI acknowledgement. Service that published
        // request directly so both shootdowns can make forward progress.
        service_tlb_local();
        arch_cpu_relax();
    }
}

void smp_tlb_shootdown(uintptr_t addr) {
    if (!sched_is_running()) {
        return;
    }

    if (!smp.shootdown_enabled || smp_online_count() <= 1) {
        return;
    }

    cpu_core_t *self = cpu_current();
    if (!self || self->id >= TLB_MAX_TARGETS) {
        return;
    }

    sched_preempt_disable();
    lock_tlb();
    unsigned long irq_flags = arch_irq_save();

    u64 targets = tlb_targets(self);
    if (!targets) {
        spin_unlock(&smp.tlb_lock);
        arch_irq_restore(irq_flags);
        sched_preempt_enable();
        return;
    }

    __atomic_store_n(&smp.tlb_addr, addr, __ATOMIC_RELEASE);
    __atomic_store_n(&smp.tlb_targets, targets, __ATOMIC_RELEASE);
    u64 seq = __atomic_load_n(&smp.tlb_seq, __ATOMIC_RELAXED) + 1;
    if (!seq) {
        panic("TLB shootdown sequence exhausted");
    }

    // Publish the sequence last. Delayed IPIs identify the request they
    // serviced through the per-CPU seen sequence and cannot ACK a later one.
    __atomic_store_n(&smp.tlb_seq, seq, __ATOMIC_RELEASE);

    send_tlb_ipis(targets);
    wait_tlb_seen(self, targets, seq);

    __atomic_store_n(&smp.tlb_targets, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&smp.tlb_addr, 0, __ATOMIC_RELEASE);

    spin_unlock(&smp.tlb_lock);
    arch_irq_restore(irq_flags);
    sched_preempt_enable();
}
