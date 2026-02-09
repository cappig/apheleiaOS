#include "apic.h"

#include <arch/arch.h>
#include <base/attributes.h>
#include <base/macros.h>
#include <log/log.h>
#include <string.h>
#include <sys/acpi.h>
#include <sys/cpu.h>
#include <x86/asm.h>
#include <x86/boot.h>
#include <x86/idt.h>
#include <x86/irq.h>
#include <x86/tsc.h>

#if defined(__i386__)
#include <x86/mm/physical.h>
#include <x86/paging32.h>
#else
#include <x86/paging64.h>
#endif

#define APIC_TIMER_DIVIDE_16 0x3
#define APIC_TIMER_CAL_MS    10

#define IOAPIC_REGSEL      0x00
#define IOAPIC_WINDOW      0x10
#define IOAPIC_REG_VER     0x01
#define IOAPIC_REDTBL_BASE 0x10

#define IOAPIC_ENTRY_MASK          (1ULL << 16)
#define IOAPIC_ENTRY_POLARITY_LOW  (1ULL << 13)
#define IOAPIC_ENTRY_TRIGGER_LEVEL (1ULL << 15)

#if defined(__i386__)
#define APIC_MMIO_BASE_32   0xfec00000U
#define APIC_MMIO_STRIDE_32 0x00010000U
#endif

typedef struct PACKED {
    u8 acpi_id;
    u8 apic_id;
    u32 flags;
} madt_lapic_t;

typedef struct PACKED {
    u8 ioapic_id;
    u8 reserved;
    u32 ioapic_addr;
    u32 gsi_base;
} madt_ioapic_t;

typedef struct PACKED {
    u8 bus;
    u8 source;
    u32 gsi;
    u16 flags;
} madt_int_override_t;

typedef struct PACKED {
    u16 reserved;
    u64 lapic_addr;
} madt_lapic_addr_t;

typedef struct PACKED {
    u16 reserved;
    u32 x2apic_id;
    u32 flags;
    u32 acpi_uid;
} madt_lapic2_t;

typedef struct {
    u8 id;
    u32 gsi_base;
    u32 int_count;
    u64 paddr;
    u64 vaddr;
    bool valid;
} ioapic_info_t;

typedef struct {
    u8 source;
    u32 gsi;
    u16 flags;
    bool valid;
} ioapic_override_t;

static bool apic_enabled = false;
static bool apic_timer_ready = false;
static u32 apic_timer_rate_hz = 0;

static u64 lapic_paddr = 0;
static volatile u32* lapic_mmio = NULL;

static u64 madt_lapic_override = 0;
static bool madt_parsed = false;

static ioapic_info_t ioapics[8] = {0};
static size_t ioapic_count = 0;
static ioapic_override_t ioapic_overrides[16] = {0};

#if defined(__i386__)
static u32 apic_mmio_next_vaddr = APIC_MMIO_BASE_32;

static page_t* _get_pdpt(void) {
    return (page_t*)(uintptr_t)(read_cr3() & ~0x1fULL);
}

static page_t* _walk_pdpt(size_t index) {
    page_t* pdpt = _get_pdpt();
    page_t entry = pdpt[index];

    if (entry & PT_PRESENT)
        return (page_t*)(uintptr_t)page_get_paddr(&pdpt[index]);

    page_t* pd = alloc_frames(1);
    memset(pd, 0, PAGE_4KIB);

    page_set_paddr(&pdpt[index], (page_t)(uintptr_t)pd);
    pdpt[index] |= PT_PRESENT | PT_WRITE;

    return pd;
}

static page_t* _walk_pd(page_t* pd, size_t index) {
    page_t entry = pd[index];

    if (entry & PT_PRESENT)
        return (page_t*)(uintptr_t)page_get_paddr(&pd[index]);

    page_t* pt = alloc_frames(1);
    memset(pt, 0, PAGE_4KIB);

    page_set_paddr(&pd[index], (page_t)(uintptr_t)pt);
    pd[index] |= PT_PRESENT | PT_WRITE;

    return pt;
}

static void _map_page(u32 vaddr, u64 paddr, u64 flags) {
    size_t lvl3 = GET_LVL3_INDEX(vaddr);
    size_t lvl2 = GET_LVL2_INDEX(vaddr);
    size_t lvl1 = GET_LVL1_INDEX(vaddr);

    page_t* pd = _walk_pdpt(lvl3);
    page_t* pt = _walk_pd(pd, lvl2);

    page_t* entry = &pt[lvl1];
    page_set_paddr(entry, (page_t)paddr);
    *entry |= (flags | PT_PRESENT) & FLAGS_MASK;

    tlb_flush(vaddr);
}

static void* _map_mmio(u64 paddr) {
    u32 vaddr = apic_mmio_next_vaddr;

    apic_mmio_next_vaddr += APIC_MMIO_STRIDE_32;

    if (apic_mmio_next_vaddr >= PHYS_WINDOW_BASE_32) {
        log_warn("apic: MMIO mapping space exhausted");
        return NULL;
    }

    _map_page(vaddr, paddr, PT_WRITE | PT_NO_CACHE);

    return (void*)(uintptr_t)vaddr;
}
#else
static void* _map_mmio(u64 paddr) {
    return arch_phys_map(paddr, PAGE_4KIB);
}
#endif

static u32 _ioapic_read_reg(const ioapic_info_t* info, u8 reg) {
    if (!info || !info->valid)
        return 0;

    volatile u32* regsel = (volatile u32*)(uintptr_t)info->vaddr;
    volatile u32* window = (volatile u32*)((uintptr_t)info->vaddr + IOAPIC_WINDOW);

    *regsel = reg;
    return *window;
}

static void _ioapic_write_reg(const ioapic_info_t* info, u8 reg, u32 value) {
    if (!info || !info->valid)
        return;

    volatile u32* regsel = (volatile u32*)(uintptr_t)info->vaddr;
    volatile u32* window = (volatile u32*)((uintptr_t)info->vaddr + IOAPIC_WINDOW);

    *regsel = reg;
    *window = value;
}

static void _ioapic_update_info(ioapic_info_t* info) {
    if (!info || !info->valid)
        return;

    u32 ver = _ioapic_read_reg(info, IOAPIC_REG_VER);
    info->int_count = ((ver >> 16) & 0xff) + 1;
}

static void _ioapic_resolve_gsi(u8 irq, u32* gsi, u16* flags) {
    if (gsi)
        *gsi = irq;
    if (flags)
        *flags = 0;

    if (irq >= ARRAY_LEN(ioapic_overrides))
        return;

    ioapic_override_t* override = &ioapic_overrides[irq];
    if (!override->valid)
        return;

    if (gsi)
        *gsi = override->gsi;
    if (flags)
        *flags = override->flags;
}

static ioapic_info_t* _ioapic_for_gsi(u32 gsi, u32* index) {
    for (size_t i = 0; i < ioapic_count; i++) {
        ioapic_info_t* info = &ioapics[i];

        if (!info->valid || !info->int_count)
            continue;

        if (gsi < info->gsi_base)
            continue;

        u32 offset = gsi - info->gsi_base;
        if (offset >= info->int_count)
            continue;

        if (index)
            *index = offset;

        return info;
    }

    return NULL;
}

static u64 _ioapic_flags_to_entry(u16 flags) {
    u64 entry = 0;

    u16 polarity = flags & 0x3;
    u16 trigger = (flags >> 2) & 0x3;

    if (polarity == 3)
        entry |= IOAPIC_ENTRY_POLARITY_LOW;

    if (trigger == 3)
        entry |= IOAPIC_ENTRY_TRIGGER_LEVEL;

    return entry;
}

static u32 _read(u32 reg) {
    if (!lapic_mmio)
        return 0;

    volatile u32* addr = (volatile u32*)((uintptr_t)lapic_mmio + reg);
    return *addr;
}

static void _write(u32 reg, u32 value) {
    if (!lapic_mmio)
        return;

    volatile u32* addr = (volatile u32*)((uintptr_t)lapic_mmio + reg);
    *addr = value;
}

static u32 _read_id(void) {
    u32 id = _read(LAPIC_ID_REG);
    return id >> 24;
}

static void _parse_madt(void) {
    madt_t* madt = (madt_t*)acpi_find_table("APIC");
    if (!madt)
        return;

    madt_lapic_override = madt->local_apic_addr;
    ioapic_count = 0;
    memset(ioapic_overrides, 0, sizeof(ioapic_overrides));

    size_t offset = 0;
    size_t found_cores = 0;
    size_t limit = madt->header.length - sizeof(madt_t);

    while (offset + sizeof(madt_entry_t) <= limit) {
        madt_entry_t* entry = (madt_entry_t*)(madt->entries + offset);
        if (!entry->length)
            break;

        switch (entry->type) {
        case MT_LOC_APIC: {
            if (entry->length >= sizeof(madt_entry_t) + sizeof(madt_lapic_t)) {
                madt_lapic_t* lapic = (madt_lapic_t*)entry->data;
                if (lapic->flags & 1U) {
                    if (found_cores < MAX_CORES) {
                        cpu_init_core(found_cores);
                        cores_local[found_cores].lapic_id = lapic->apic_id;
                        found_cores++;
                    }
                }
            }
            break;
        }
        case MT_LOC_APIC2: {
            if (entry->length >= sizeof(madt_entry_t) + sizeof(madt_lapic2_t)) {
                madt_lapic2_t* lapic = (madt_lapic2_t*)entry->data;
                if (lapic->flags & 1U) {
                    if (found_cores < MAX_CORES) {
                        cpu_init_core(found_cores);
                        cores_local[found_cores].lapic_id = lapic->x2apic_id;
                        found_cores++;
                    }
                }
            }
            break;
        }
        case MT_IO_APIC: {
            if (entry->length >= sizeof(madt_entry_t) + sizeof(madt_ioapic_t)) {
                madt_ioapic_t* io = (madt_ioapic_t*)entry->data;

                if (ioapic_count < ARRAY_LEN(ioapics)) {
                    ioapic_info_t* info = &ioapics[ioapic_count++];
                    info->id = io->ioapic_id;
                    info->gsi_base = io->gsi_base;
                    info->paddr = io->ioapic_addr;
                    info->vaddr = (u64)(uintptr_t)_map_mmio(io->ioapic_addr);
                    info->valid = (info->vaddr != 0);

                    if (info->valid)
                        _ioapic_update_info(info);
                }
            }
            break;
        }
        case MT_IO_APIC_INT_SRC: {
            if (entry->length >= sizeof(madt_entry_t) + sizeof(madt_int_override_t)) {
                madt_int_override_t* override = (madt_int_override_t*)entry->data;
                if (override->bus == 0 && override->source < ARRAY_LEN(ioapic_overrides)) {
                    ioapic_overrides[override->source].source = override->source;
                    ioapic_overrides[override->source].gsi = override->gsi;
                    ioapic_overrides[override->source].flags = override->flags;
                    ioapic_overrides[override->source].valid = true;
                }
            }
            break;
        }
        case MT_LOC_APIC_ADDR: {
            if (entry->length >= sizeof(madt_entry_t) + sizeof(madt_lapic_addr_t)) {
                madt_lapic_addr_t* addr = (madt_lapic_addr_t*)entry->data;
                madt_lapic_override = addr->lapic_addr;
            }
            break;
        }
        default:
            break;
        }

        offset += entry->length;
    }

    if (found_cores > 0) {
        core_count = found_cores;
        cpu_set_current(&cores_local[0]);
    }

    madt_parsed = true;
}

static u32 _calibrate_timer(u32 hz) {
    if (!hz)
        return 0;

    if (!tsc_khz())
        return 0;

    _write(LAPIC_TIMER_DIVIDE_REG, APIC_TIMER_DIVIDE_16);
    _write(LAPIC_LVT_TIMER_REG, LAPIC_LVT_MASK);

    const u32 begin = 0xffffffffU;
    _write(LAPIC_TIMER_ICOUNT_REG, begin);

    tsc_spin(APIC_TIMER_CAL_MS);

    u32 end = _read(LAPIC_TIMER_CCOUNT_REG);
    u32 elapsed = begin - end;
    if (!elapsed)
        return 0;

    u64 counts_per_ms = elapsed / APIC_TIMER_CAL_MS;
    if (!counts_per_ms)
        return 0;

    u64 initial = (counts_per_ms * 1000) / hz;

    if (!initial || initial > 0xffffffffULL)
        return 0;

    return (u32)initial;
}

bool apic_init(void) {
    cpuid_regs_t regs = {0};
    cpuid(1, &regs);

    bool has_apic = (regs.edx & (1U << 9)) != 0;
    bool has_msr = (regs.edx & (1U << 5)) != 0;

    if (!has_apic || !has_msr) {
        log_warn("apic: not supported (apic=%u msr=%u)", (u32)has_apic, (u32)has_msr);
        return false;
    }

    if (!madt_parsed)
        _parse_madt();

    u64 base = read_msr(APIC_BASE_MSR);
    lapic_paddr = base & APIC_MSR_ADDR_MASK;

    if (madt_lapic_override)
        lapic_paddr = madt_lapic_override & APIC_MSR_ADDR_MASK;

    base &= ~APIC_MSR_ADDR_MASK;
    base |= lapic_paddr & APIC_MSR_ADDR_MASK;
    base |= APIC_MSR_APIC_ENABLE;

    write_msr(APIC_BASE_MSR, base);

    lapic_mmio = _map_mmio(lapic_paddr);
    if (!lapic_mmio) {
        log_warn("apic: failed to map LAPIC");
        return false;
    }

    _write(LAPIC_SPURIOUS_REG, INT_SPURIOUS | (1U << 8));
    _write(LAPIC_LVT_LINT0_REG, LAPIC_LVT_MASK);
    _write(LAPIC_LVT_LINT1_REG, LAPIC_LVT_MASK);

    u32 id = _read_id();
    cpu_core_t* core = cpu_current();
    if (core)
        core->lapic_id = id;

    apic_enabled = true;
    log_info("apic: local APIC enabled (id=%u)", id);

    return true;
}

bool apic_timer_init(u32 hz) {
    if (!apic_enabled)
        return false;

    u32 initial = _calibrate_timer(hz);
    if (!initial)
        return false;

    u32 lvt = IRQ_INT(IRQ_SYSTEM_TIMER) | LAPIC_TIMER_PERIODIC | LAPIC_LVT_MASK;
    _write(LAPIC_LVT_TIMER_REG, lvt);
    _write(LAPIC_TIMER_ICOUNT_REG, initial);

    apic_timer_rate_hz = hz;
    apic_timer_ready = true;

    return true;
}

void apic_timer_enable(void) {
    if (!apic_timer_ready)
        return;

    u32 lvt = _read(LAPIC_LVT_TIMER_REG);
    lvt &= ~LAPIC_LVT_MASK;
    _write(LAPIC_LVT_TIMER_REG, lvt);
}

void apic_timer_disable(void) {
    if (!apic_timer_ready)
        return;

    u32 lvt = _read(LAPIC_LVT_TIMER_REG);
    lvt |= LAPIC_LVT_MASK;
    _write(LAPIC_LVT_TIMER_REG, lvt);
}

bool apic_timer_active(void) {
    return apic_timer_ready;
}

u32 apic_timer_hz(void) {
    return apic_timer_rate_hz;
}

bool ioapic_available(void) {
    if (!ioapic_count)
        return false;

    for (size_t i = 0; i < ioapic_count; i++) {
        if (ioapics[i].valid && ioapics[i].int_count)
            return true;
    }

    return false;
}

void ioapic_mask_all(void) {
    for (size_t i = 0; i < ioapic_count; i++) {
        ioapic_info_t* info = &ioapics[i];
        if (!info->valid || !info->int_count)
            continue;

        for (u32 pin = 0; pin < info->int_count; pin++) {
            u8 reg = (u8)(IOAPIC_REDTBL_BASE + pin * 2);
            u32 low = _ioapic_read_reg(info, reg);
            low |= (u32)IOAPIC_ENTRY_MASK;
            _ioapic_write_reg(info, reg, low);
        }
    }
}

void ioapic_mask_irq(u8 irq, bool masked) {
    u32 gsi = 0;

    _ioapic_resolve_gsi(irq, &gsi, NULL);

    u32 index = 0;
    ioapic_info_t* info = _ioapic_for_gsi(gsi, &index);
    if (!info)
        return;

    u8 reg = (u8)(IOAPIC_REDTBL_BASE + index * 2);
    u32 low = _ioapic_read_reg(info, reg);

    if (masked)
        low |= (u32)IOAPIC_ENTRY_MASK;
    else
        low &= ~(u32)IOAPIC_ENTRY_MASK;

    _ioapic_write_reg(info, reg, low);
}

bool ioapic_route_irq(u8 irq, u8 vector, u32 dest_apic) {
    u32 gsi = 0;
    u16 flags = 0;

    _ioapic_resolve_gsi(irq, &gsi, &flags);

    u32 index = 0;
    ioapic_info_t* info = _ioapic_for_gsi(gsi, &index);
    if (!info)
        return false;

    u64 entry = vector;
    entry |= ((u64)(dest_apic & 0xff) << 56);
    entry |= _ioapic_flags_to_entry(flags);
    entry &= ~IOAPIC_ENTRY_MASK;

    u8 reg = (u8)(IOAPIC_REDTBL_BASE + index * 2);
    _ioapic_write_reg(info, (u8)(reg + 1), (u32)(entry >> 32));
    _ioapic_write_reg(info, reg, (u32)(entry & 0xffffffff));

    return true;
}

void lapic_end_int(void) {
    if (!apic_enabled)
        return;

    _write(LAPIC_EOI_REG, 0);
}

u32 lapic_id(void) {
    return _read_id();
}
