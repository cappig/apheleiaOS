#include <arch/arch.h>
#include <base/types.h>
#include <stdbool.h>
#include <stddef.h>

#if !defined(__i386__)
#error "atomic32.c is only meant for 32-bit x86 builds"
#endif

// clang lowers some 64-bit atomics in i386 freestanding code to libatomic
// calls. The kernel cannot link libatomic, so keep the small operations we use
// here and serialize them with a raw lock

static volatile int atomic64_lock = 0;

static unsigned long atomic64_lock_save(void) {
    unsigned long flags = arch_irq_save();

    while (__sync_lock_test_and_set(&atomic64_lock, 1)) {
        while (__atomic_load_n(&atomic64_lock, __ATOMIC_RELAXED)) {
            arch_cpu_relax();
        }
    }

    return flags;
}

static void atomic64_unlock_restore(unsigned long flags) {
    __sync_lock_release(&atomic64_lock);
    arch_irq_restore(flags);
}

static void atomic_copy(void *dst, const void *src, size_t size) {
    unsigned char *out = dst;
    const unsigned char *in = src;

    for (size_t i = 0; i < size; i++) {
        out[i] = in[i];
    }
}

u64 __atomic_load_8(const volatile void *ptr, int memorder) {
    (void)memorder;

    unsigned long flags = atomic64_lock_save();
    u64 value = *(const volatile u64 *)ptr;
    atomic64_unlock_restore(flags);

    return value;
}

void __atomic_store_8(volatile void *ptr, u64 value, int memorder) {
    (void)memorder;

    unsigned long flags = atomic64_lock_save();
    *(volatile u64 *)ptr = value;
    atomic64_unlock_restore(flags);
}

u64 __atomic_fetch_add_8(volatile void *ptr, u64 value, int memorder) {
    (void)memorder;

    unsigned long flags = atomic64_lock_save();
    volatile u64 *p = ptr;
    u64 old = *p;
    *p = old + value;
    atomic64_unlock_restore(flags);

    return old;
}

u64 __atomic_fetch_sub_8(volatile void *ptr, u64 value, int memorder) {
    (void)memorder;

    unsigned long flags = atomic64_lock_save();
    volatile u64 *p = ptr;
    u64 old = *p;
    *p = old - value;
    atomic64_unlock_restore(flags);

    return old;
}

u64 __atomic_fetch_or_8(volatile void *ptr, u64 value, int memorder) {
    (void)memorder;

    unsigned long flags = atomic64_lock_save();
    volatile u64 *p = ptr;
    u64 old = *p;
    *p = old | value;
    atomic64_unlock_restore(flags);

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

    unsigned long flags = atomic64_lock_save();
    volatile u64 *p = ptr;
    u64 *exp = expected;
    u64 old = *p;

    if (old == *exp) {
        *p = desired;
        atomic64_unlock_restore(flags);
        return true;
    }

    *exp = old;
    atomic64_unlock_restore(flags);
    return false;
}

void x86_atomic_load(size_t size, const volatile void *ptr, void *ret, int memorder) __asm__("__atomic_load");

void x86_atomic_load(size_t size, const volatile void *ptr, void *ret, int memorder) {
    (void)memorder;

    unsigned long flags = atomic64_lock_save();
    atomic_copy(ret, (const void *)ptr, size);
    atomic64_unlock_restore(flags);
}

bool x86_atomic_compare_exchange(
    size_t size,
    volatile void *ptr,
    void *expected,
    void *desired,
    int success_memorder,
    int failure_memorder
) __asm__("__atomic_compare_exchange");

bool x86_atomic_compare_exchange(
    size_t size,
    volatile void *ptr,
    void *expected,
    void *desired,
    int success_memorder,
    int failure_memorder
) {
    (void)success_memorder;
    (void)failure_memorder;

    unsigned long flags = atomic64_lock_save();
    const unsigned char *cur = (const unsigned char *)ptr;
    const unsigned char *exp = expected;
    bool equal = true;

    for (size_t i = 0; i < size; i++) {
        if (cur[i] != exp[i]) {
            equal = false;
            break;
        }
    }

    if (equal) {
        atomic_copy((void *)ptr, desired, size);
        atomic64_unlock_restore(flags);
        return true;
    }

    atomic_copy(expected, (const void *)ptr, size);
    atomic64_unlock_restore(flags);
    return false;
}
