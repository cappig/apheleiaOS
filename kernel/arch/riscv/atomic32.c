#include <arch/arch.h>
#include <base/types.h>
#include <stdbool.h>

#if __riscv_xlen != 32
#error "atomic32.c is only meant for rv32 builds"
#endif

/*
 * rv32 currently runs single-hart only. Mask interrupts around 64-bit
 * read-modify-write sequences so compiler-emitted __atomic_*_8 calls can link
 * and behave consistently without dragging in libatomic.
 */

u64 __atomic_load_8(const volatile void *ptr, int memorder) {
    (void)memorder;
    const volatile u64 *p = (const volatile u64 *)ptr;

    unsigned long flags = arch_irq_save();
    u64 value = *p;
    arch_irq_restore(flags);
    return value;
}

void __atomic_store_8(volatile void *ptr, u64 value, int memorder) {
    (void)memorder;
    volatile u64 *p = (volatile u64 *)ptr;

    unsigned long flags = arch_irq_save();
    *p = value;
    arch_irq_restore(flags);
}

u64 __atomic_fetch_add_8(volatile void *ptr, u64 value, int memorder) {
    (void)memorder;
    volatile u64 *p = (volatile u64 *)ptr;

    unsigned long flags = arch_irq_save();
    u64 old = *p;
    *p = old + value;
    arch_irq_restore(flags);
    return old;
}

u64 __atomic_fetch_or_8(volatile void *ptr, u64 value, int memorder) {
    (void)memorder;
    volatile u64 *p = (volatile u64 *)ptr;

    unsigned long flags = arch_irq_save();
    u64 old = *p;
    *p = old | value;
    arch_irq_restore(flags);
    return old;
}

bool __atomic_compare_exchange_8(
    volatile void *ptr,
    void *expected,
    u64 desired,
    bool weak,
    int success_memorder,
    int failure_memorder
) {
    (void)weak;
    (void)success_memorder;
    (void)failure_memorder;

    volatile u64 *p = (volatile u64 *)ptr;
    u64 *exp = (u64 *)expected;

    unsigned long flags = arch_irq_save();
    u64 old = *p;
    if (old == *exp) {
        *p = desired;
        arch_irq_restore(flags);
        return true;
    }

    *exp = old;
    arch_irq_restore(flags);
    return false;
}

u64 __sync_fetch_and_add_8(volatile void *ptr, u64 value) {
    volatile u64 *p = (volatile u64 *)ptr;
    unsigned long flags = arch_irq_save();
    u64 old = *p;
    *p = old + value;
    arch_irq_restore(flags);
    return old;
}

u64 __sync_val_compare_and_swap_8(
    volatile void *ptr,
    u64 old_value,
    u64 new_value
) {
    volatile u64 *p = (volatile u64 *)ptr;
    unsigned long flags = arch_irq_save();
    u64 old = *p;
    if (old == old_value) {
        *p = new_value;
    }
    arch_irq_restore(flags);
    return old;
}

bool __sync_bool_compare_and_swap_8(
    volatile void *ptr,
    u64 old_value,
    u64 new_value
) {
    volatile u64 *p = (volatile u64 *)ptr;
    unsigned long flags = arch_irq_save();
    bool swapped = false;
    if (*p == old_value) {
        *p = new_value;
        swapped = true;
    }
    arch_irq_restore(flags);
    return swapped;
}
