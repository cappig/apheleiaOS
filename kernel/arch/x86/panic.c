#include <arch/arch.h>
#include <arch/signal.h>
#include <inttypes.h>
#include <log/log.h>

#include "x86/asm.h"

static volatile u32 panic_active = 0;

#if defined(__x86_64__)
typedef struct {
    u16 ds;
    u16 es;
    u16 fs;
    u16 gs;
    u64 cr0;
    u64 cr2;
    u64 cr3;
    u64 cr4;
} segs_ctrl_t;

static inline u16 _read_ds(void) {
    u16 value = 0;
    __asm__ volatile("mov %%ds, %0" : "=r"(value));
    return value;
}

static inline u16 _read_es(void) {
    u16 value = 0;
    __asm__ volatile("mov %%es, %0" : "=r"(value));
    return value;
}

static inline u16 _read_fs(void) {
    u16 value = 0;
    __asm__ volatile("mov %%fs, %0" : "=r"(value));
    return value;
}

static inline u16 _read_gs(void) {
    u16 value = 0;
    __asm__ volatile("mov %%gs, %0" : "=r"(value));
    return value;
}

static inline u64 _read_stack_ptr_live(void) {
    u64 value = 0;
    __asm__ volatile("mov %%rsp, %0" : "=r"(value));
    return value;
}

static inline u16 _read_ss(void) {
    u16 value = 0;
    __asm__ volatile("mov %%ss, %0" : "=r"(value));
    return value;
}

static void _capture_segs_ctrl(segs_ctrl_t *out) {
    if (!out) {
        return;
    }

    out->ds = _read_ds();
    out->es = _read_es();
    out->fs = _read_fs();
    out->gs = _read_gs();
    out->cr0 = read_cr0();
    out->cr2 = read_cr2();
    out->cr3 = read_cr3();
    out->cr4 = read_cr4();
}

static void _log_segs_ctrl(const segs_ctrl_t *sc) {
    if (!sc) {
        return;
    }

    log_fatal(
        "  DS=%#06" PRIx16 " ES=%#06" PRIx16 " FS=%#06" PRIx16 " GS=%#06" PRIx16,
        sc->ds,
        sc->es,
        sc->fs,
        sc->gs
    );
    log_fatal(
        "  CR0=%#018" PRIx64 " CR2=%#018" PRIx64
        " CR3=%#018" PRIx64 " CR4=%#018" PRIx64,
        sc->cr0,
        sc->cr2,
        sc->cr3,
        sc->cr4
    );
}
#else
typedef struct {
    u16 ds;
    u16 es;
    u16 fs;
    u16 gs;
    u32 cr0;
    u32 cr2;
    u32 cr3;
    u32 cr4;
} segs_ctrl_t;

static inline u16 _read_ds(void) {
    u16 value = 0;
    __asm__ volatile("mov %%ds, %0" : "=r"(value));
    return value;
}

static inline u16 _read_es(void) {
    u16 value = 0;
    __asm__ volatile("mov %%es, %0" : "=r"(value));
    return value;
}

static inline u16 _read_fs(void) {
    u16 value = 0;
    __asm__ volatile("mov %%fs, %0" : "=r"(value));
    return value;
}

static inline u16 _read_gs(void) {
    u16 value = 0;
    __asm__ volatile("mov %%gs, %0" : "=r"(value));
    return value;
}

static inline u32 _read_stack_ptr_live(void) {
    u32 value = 0;
    __asm__ volatile("mov %%esp, %0" : "=r"(value));
    return value;
}

static inline u16 _read_ss(void) {
    u16 value = 0;
    __asm__ volatile("mov %%ss, %0" : "=r"(value));
    return value;
}

static void _capture_segs_ctrl(segs_ctrl_t *out) {
    if (!out) {
        return;
    }

    out->ds = _read_ds();
    out->es = _read_es();
    out->fs = _read_fs();
    out->gs = _read_gs();
    out->cr0 = read_cr0();
    out->cr2 = read_cr2();
    out->cr3 = (u32)read_cr3();
    out->cr4 = read_cr4();
}

static void _log_segs_ctrl(const segs_ctrl_t *sc) {
    if (!sc) {
        return;
    }

    log_fatal(
        "  DS=%#06" PRIx16 " ES=%#06" PRIx16 " FS=%#06" PRIx16 " GS=%#06" PRIx16,
        sc->ds,
        sc->es,
        sc->fs,
        sc->gs
    );
    log_fatal(
        "  CR0=%#010" PRIx32 " CR2=%#010" PRIx32
        " CR3=%#010" PRIx32 " CR4=%#010" PRIx32,
        sc->cr0,
        sc->cr2,
        sc->cr3,
        sc->cr4
    );
}
#endif

static void _dump_regs_from_state(const arch_int_state_t *state) {
    if (!state) {
        return;
    }

    bool user_frame = arch_signal_is_user(state);

#if defined(__x86_64__)
    u64 stack_ptr = user_frame ? state->s_regs.rsp : _read_stack_ptr_live();
    u64 stack_seg = user_frame ? state->s_regs.ss : (u64)_read_ss();

    log_fatal("register dump");
    log_fatal(
        "  RIP=%#018" PRIx64 " RSP=%#018" PRIx64 " RFLAGS=%#018" PRIx64
        " CS=%#06" PRIx64 " SS=%#06" PRIx64,
        state->s_regs.rip,
        stack_ptr,
        state->s_regs.rflags,
        state->s_regs.cs,
        stack_seg
    );
    log_fatal(
        "  RSI=%#018" PRIx64 " RDI=%#018" PRIx64 " RBP=%#018" PRIx64,
        state->g_regs.rsi,
        state->g_regs.rdi,
        state->g_regs.rbp
    );
    log_fatal(
        "  RAX=%#018" PRIx64 " RBX=%#018" PRIx64
        " RCX=%#018" PRIx64 " RDX=%#018" PRIx64,
        state->g_regs.rax,
        state->g_regs.rbx,
        state->g_regs.rcx,
        state->g_regs.rdx
    );
    log_fatal(
        "   R8=%#018" PRIx64 "  R9=%#018" PRIx64
        " R10=%#018" PRIx64 " R11=%#018" PRIx64,
        state->g_regs.r8,
        state->g_regs.r9,
        state->g_regs.r10,
        state->g_regs.r11
    );
    log_fatal(
        "  R12=%#018" PRIx64 " R13=%#018" PRIx64
        " R14=%#018" PRIx64 " R15=%#018" PRIx64,
        state->g_regs.r12,
        state->g_regs.r13,
        state->g_regs.r14,
        state->g_regs.r15
    );
#else
    u32 stack_ptr = user_frame ? state->s_regs.esp : _read_stack_ptr_live();
    u32 stack_seg = user_frame ? state->s_regs.ss : (u32)_read_ss();

    log_fatal("register dump");
    log_fatal(
        "  EIP=%#010" PRIx32 " ESP=%#010" PRIx32 " EFLAGS=%#010" PRIx32
        " CS=%#06" PRIx32 " SS=%#06" PRIx32,
        state->s_regs.eip,
        stack_ptr,
        state->s_regs.eflags,
        state->s_regs.cs,
        stack_seg
    );
    log_fatal(
        "  ESI=%#010" PRIx32 " EDI=%#010" PRIx32 " EBP=%#010" PRIx32,
        state->g_regs.esi,
        state->g_regs.edi,
        state->g_regs.ebp
    );
    log_fatal(
        "  EAX=%#010" PRIx32 " EBX=%#010" PRIx32
        " ECX=%#010" PRIx32 " EDX=%#010" PRIx32,
        state->g_regs.eax,
        state->g_regs.ebx,
        state->g_regs.ecx,
        state->g_regs.edx
    );
#endif
}

#if defined(__x86_64__)
typedef struct {
    u64 rax;
    u64 rbx;
    u64 rcx;
    u64 rdx;
    u64 rsi;
    u64 rdi;
    u64 rbp;
    u64 rsp;
    u64 r8;
    u64 r9;
    u64 r10;
    u64 r11;
    u64 r12;
    u64 r13;
    u64 r14;
    u64 r15;
    u64 rflags;
    u16 cs;
    u16 ss;
    u64 rip;
} reg_snapshot_t;

static void _capture_regs(reg_snapshot_t *out) {
    if (!out) {
        return;
    }

    __asm__ volatile(
        "mov %%rax, %0\n\t"
        "mov %%rbx, %1\n\t"
        "mov %%rcx, %2\n\t"
        "mov %%rdx, %3\n\t"
        "mov %%rsi, %4\n\t"
        "mov %%rdi, %5\n\t"
        "mov %%rbp, %6\n\t"
        "mov %%rsp, %7\n\t"
        "mov %%r8,  %8\n\t"
        "mov %%r9,  %9\n\t"
        "mov %%r10, %10\n\t"
        "mov %%r11, %11\n\t"
        "mov %%r12, %12\n\t"
        "mov %%r13, %13\n\t"
        "mov %%r14, %14\n\t"
        "mov %%r15, %15\n\t"
        "pushfq\n\t"
        "popq %16\n\t"
        "mov %%cs, %17\n\t"
        "mov %%ss, %18\n\t"
        "call 1f\n\t"
        "1: popq %19\n\t"
        : "=m"(out->rax),
          "=m"(out->rbx),
          "=m"(out->rcx),
          "=m"(out->rdx),
          "=m"(out->rsi),
          "=m"(out->rdi),
          "=m"(out->rbp),
          "=m"(out->rsp),
          "=m"(out->r8),
          "=m"(out->r9),
          "=m"(out->r10),
          "=m"(out->r11),
          "=m"(out->r12),
          "=m"(out->r13),
          "=m"(out->r14),
          "=m"(out->r15),
          "=m"(out->rflags),
          "=m"(out->cs),
          "=m"(out->ss),
          "=m"(out->rip)
        :
        : "memory"
    );
}

static void _dump_regs_snapshot(void) {
    reg_snapshot_t regs = {0};
    _capture_regs(&regs);

    log_fatal("register dump");
    log_fatal(
        "  RIP=%#018" PRIx64 " RSP=%#018" PRIx64 " RFLAGS=%#018" PRIx64
        " CS=%#06" PRIx64 " SS=%#06" PRIx64,
        regs.rip,
        regs.rsp,
        regs.rflags,
        (u64)regs.cs,
        (u64)regs.ss
    );
    log_fatal(
        "  RSI=%#018" PRIx64 " RDI=%#018" PRIx64 " RBP=%#018" PRIx64,
        regs.rsi,
        regs.rdi,
        regs.rbp
    );
    log_fatal(
        "  RAX=%#018" PRIx64 " RBX=%#018" PRIx64
        " RCX=%#018" PRIx64 " RDX=%#018" PRIx64,
        regs.rax,
        regs.rbx,
        regs.rcx,
        regs.rdx
    );
    log_fatal(
        "   R8=%#018" PRIx64 "  R9=%#018" PRIx64
        " R10=%#018" PRIx64 " R11=%#018" PRIx64,
        regs.r8,
        regs.r9,
        regs.r10,
        regs.r11
    );
    log_fatal(
        "  R12=%#018" PRIx64 " R13=%#018" PRIx64
        " R14=%#018" PRIx64 " R15=%#018" PRIx64,
        regs.r12,
        regs.r13,
        regs.r14,
        regs.r15
    );
}
#else
typedef struct {
    u32 eax;
    u32 ebx;
    u32 ecx;
    u32 edx;
    u32 esi;
    u32 edi;
    u32 ebp;
    u32 esp;
    u32 eflags;
    u16 cs;
    u16 ss;
    u32 eip;
} reg_snapshot_t;

static void _capture_regs(reg_snapshot_t *out) {
    if (!out) {
        return;
    }

    __asm__ volatile(
        "mov %%eax, %0\n\t"
        "mov %%ebx, %1\n\t"
        "mov %%ecx, %2\n\t"
        "mov %%edx, %3\n\t"
        "mov %%esi, %4\n\t"
        "mov %%edi, %5\n\t"
        "mov %%ebp, %6\n\t"
        "mov %%esp, %7\n\t"
        "pushfl\n\t"
        "popl %8\n\t"
        "mov %%cs, %9\n\t"
        "mov %%ss, %10\n\t"
        "call 1f\n\t"
        "1: popl %11\n\t"
        : "=m"(out->eax),
          "=m"(out->ebx),
          "=m"(out->ecx),
          "=m"(out->edx),
          "=m"(out->esi),
          "=m"(out->edi),
          "=m"(out->ebp),
          "=m"(out->esp),
          "=m"(out->eflags),
          "=m"(out->cs),
          "=m"(out->ss),
          "=m"(out->eip)
        :
        : "memory"
    );
}

static void _dump_regs_snapshot(void) {
    reg_snapshot_t regs = {0};
    _capture_regs(&regs);

    log_fatal("register dump");
    log_fatal(
        "  EIP=%#010" PRIx32 " ESP=%#010" PRIx32 " EFLAGS=%#010" PRIx32
        " CS=%#06" PRIx32 " SS=%#06" PRIx32,
        regs.eip,
        regs.esp,
        regs.eflags,
        (u32)regs.cs,
        (u32)regs.ss
    );
    log_fatal(
        "  ESI=%#010" PRIx32 " EDI=%#010" PRIx32 " EBP=%#010" PRIx32,
        regs.esi,
        regs.edi,
        regs.ebp
    );
    log_fatal(
        "  EAX=%#010" PRIx32 " EBX=%#010" PRIx32
        " ECX=%#010" PRIx32 " EDX=%#010" PRIx32,
        regs.eax,
        regs.ebx,
        regs.ecx,
        regs.edx
    );
}
#endif

void arch_dump_registers(const arch_int_state_t *state) {
    segs_ctrl_t sc = {0};
    _capture_segs_ctrl(&sc);

    if (state) {
        _dump_regs_from_state(state);
        _log_segs_ctrl(&sc);
        return;
    }

    _dump_regs_snapshot();
    _log_segs_ctrl(&sc);
}

bool panic_in_progress(void) {
    return __atomic_load_n(&panic_active, __ATOMIC_ACQUIRE) != 0;
}

void panic_prepare(void) {
    disable_interrupts();

    if (__atomic_exchange_n(&panic_active, 1, __ATOMIC_ACQ_REL) == 0) {
        arch_panic_enter();
    }
}

void panic_halt(void) {
    halt();
}
