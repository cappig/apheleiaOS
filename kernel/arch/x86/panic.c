#include <arch/arch.h>
#include <inttypes.h>
#include <stddef.h>
#include <log/log.h>

#include "x86/asm.h"

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
        (unsigned int)sc->cr0,
        (unsigned int)sc->cr2,
        (unsigned int)sc->cr3,
        (unsigned int)sc->cr4
    );
}
#endif

static void _dump_regs_from_state(const arch_int_state_t *state) {
    if (!state) {
        return;
    }

#if defined(__x86_64__)
    log_fatal("register dump");
    log_fatal(
        "  RIP=%#018" PRIx64 " RSP=%#018" PRIx64 " RFLAGS=%#018" PRIx64
        " CS=%#06" PRIx64 " SS=%#06" PRIx64,
        state->s_regs.rip,
        state->s_regs.rsp,
        state->s_regs.rflags,
        state->s_regs.cs,
        state->s_regs.ss
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
    log_fatal("register dump");
    log_fatal(
        "  EIP=%#010" PRIx32 " ESP=%#010" PRIx32 " EFLAGS=%#010" PRIx32
        " CS=%#06" PRIx32 " SS=%#06" PRIx32,
        (unsigned int)state->s_regs.eip,
        (unsigned int)state->s_regs.esp,
        (unsigned int)state->s_regs.eflags,
        (unsigned int)state->s_regs.cs,
        (unsigned int)state->s_regs.ss
    );
    log_fatal(
        "  ESI=%#010" PRIx32 " EDI=%#010" PRIx32 " EBP=%#010" PRIx32,
        (unsigned int)state->g_regs.esi,
        (unsigned int)state->g_regs.edi,
        (unsigned int)state->g_regs.ebp
    );
    log_fatal(
        "  EAX=%#010" PRIx32 " EBX=%#010" PRIx32
        " ECX=%#010" PRIx32 " EDX=%#010" PRIx32,
        (unsigned int)state->g_regs.eax,
        (unsigned int)state->g_regs.ebx,
        (unsigned int)state->g_regs.ecx,
        (unsigned int)state->g_regs.edx
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
        "mov %%rax, %c[rax](%0)\n\t"
        "mov %%rbx, %c[rbx](%0)\n\t"
        "mov %%rcx, %c[rcx](%0)\n\t"
        "mov %%rdx, %c[rdx](%0)\n\t"
        "mov %%rsi, %c[rsi](%0)\n\t"
        "mov %%rdi, %c[rdi](%0)\n\t"
        "mov %%rbp, %c[rbp](%0)\n\t"
        "mov %%rsp, %c[rsp](%0)\n\t"
        "mov %%r8,  %c[r8](%0)\n\t"
        "mov %%r9,  %c[r9](%0)\n\t"
        "mov %%r10, %c[r10](%0)\n\t"
        "mov %%r11, %c[r11](%0)\n\t"
        "mov %%r12, %c[r12](%0)\n\t"
        "mov %%r13, %c[r13](%0)\n\t"
        "mov %%r14, %c[r14](%0)\n\t"
        "mov %%r15, %c[r15](%0)\n\t"
        "pushfq\n\t"
        "popq %c[rflags](%0)\n\t"
        "mov %%cs, %c[cs](%0)\n\t"
        "mov %%ss, %c[ss](%0)\n\t"
        "call 1f\n\t"
        "1: popq %c[rip](%0)\n\t"
        :
        : "r"(out),
          [rax] "i"(offsetof(reg_snapshot_t, rax)),
          [rbx] "i"(offsetof(reg_snapshot_t, rbx)),
          [rcx] "i"(offsetof(reg_snapshot_t, rcx)),
          [rdx] "i"(offsetof(reg_snapshot_t, rdx)),
          [rsi] "i"(offsetof(reg_snapshot_t, rsi)),
          [rdi] "i"(offsetof(reg_snapshot_t, rdi)),
          [rbp] "i"(offsetof(reg_snapshot_t, rbp)),
          [rsp] "i"(offsetof(reg_snapshot_t, rsp)),
          [r8] "i"(offsetof(reg_snapshot_t, r8)),
          [r9] "i"(offsetof(reg_snapshot_t, r9)),
          [r10] "i"(offsetof(reg_snapshot_t, r10)),
          [r11] "i"(offsetof(reg_snapshot_t, r11)),
          [r12] "i"(offsetof(reg_snapshot_t, r12)),
          [r13] "i"(offsetof(reg_snapshot_t, r13)),
          [r14] "i"(offsetof(reg_snapshot_t, r14)),
          [r15] "i"(offsetof(reg_snapshot_t, r15)),
          [rflags] "i"(offsetof(reg_snapshot_t, rflags)),
          [cs] "i"(offsetof(reg_snapshot_t, cs)),
          [ss] "i"(offsetof(reg_snapshot_t, ss)),
          [rip] "i"(offsetof(reg_snapshot_t, rip))
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
        "mov %%eax, %c[eax](%0)\n\t"
        "mov %%ebx, %c[ebx](%0)\n\t"
        "mov %%ecx, %c[ecx](%0)\n\t"
        "mov %%edx, %c[edx](%0)\n\t"
        "mov %%esi, %c[esi](%0)\n\t"
        "mov %%edi, %c[edi](%0)\n\t"
        "mov %%ebp, %c[ebp](%0)\n\t"
        "mov %%esp, %c[esp](%0)\n\t"
        "pushfl\n\t"
        "popl %c[eflags](%0)\n\t"
        "mov %%cs, %c[cs](%0)\n\t"
        "mov %%ss, %c[ss](%0)\n\t"
        "call 1f\n\t"
        "1: popl %c[eip](%0)\n\t"
        :
        : "r"(out),
          [eax] "i"(offsetof(reg_snapshot_t, eax)),
          [ebx] "i"(offsetof(reg_snapshot_t, ebx)),
          [ecx] "i"(offsetof(reg_snapshot_t, ecx)),
          [edx] "i"(offsetof(reg_snapshot_t, edx)),
          [esi] "i"(offsetof(reg_snapshot_t, esi)),
          [edi] "i"(offsetof(reg_snapshot_t, edi)),
          [ebp] "i"(offsetof(reg_snapshot_t, ebp)),
          [esp] "i"(offsetof(reg_snapshot_t, esp)),
          [eflags] "i"(offsetof(reg_snapshot_t, eflags)),
          [cs] "i"(offsetof(reg_snapshot_t, cs)),
          [ss] "i"(offsetof(reg_snapshot_t, ss)),
          [eip] "i"(offsetof(reg_snapshot_t, eip))
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
        (unsigned int)regs.eip,
        (unsigned int)regs.esp,
        (unsigned int)regs.eflags,
        (unsigned int)regs.cs,
        (unsigned int)regs.ss
    );
    log_fatal(
        "  ESI=%#010" PRIx32 " EDI=%#010" PRIx32 " EBP=%#010" PRIx32,
        (unsigned int)regs.esi,
        (unsigned int)regs.edi,
        (unsigned int)regs.ebp
    );
    log_fatal(
        "  EAX=%#010" PRIx32 " EBX=%#010" PRIx32
        " ECX=%#010" PRIx32 " EDX=%#010" PRIx32,
        (unsigned int)regs.eax,
        (unsigned int)regs.ebx,
        (unsigned int)regs.ecx,
        (unsigned int)regs.edx
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

void panic_prepare(void) {
    disable_interrupts();
    arch_panic_enter();
}

void panic_halt(void) {
    halt();
}
