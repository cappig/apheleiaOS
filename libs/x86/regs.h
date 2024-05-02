#pragma once

#include <base/attributes.h>
#include <base/types.h>


// https://en.wikipedia.org/wiki/FLAGS_register
#define FLAG_CF 0x0001
#define FLAG_ZF 0x0040

// 32 bit registers in the order of popping
// https://faydoc.tripod.com/cpu/popa.htm
typedef union {
    struct {
        u32 edi;
        u32 esi;
        u32 ebp;
        u32 _esp; // esp gets discarded
        u32 ebx;
        u32 edx;
        u32 ecx;
        u32 eax;

        u32 eflags;
    };
    struct {
        u16 di, hdi;
        u16 si, hsi;
        u16 bp, hbp;
        u16 _sp, _hsp;
        u16 bx, hbx;
        u16 dx, hdx;
        u16 cx, hcx;
        u16 ax, hax;

        u16 flags, hflags;

        u16 gs;
        u16 fs;
        u16 es;
        u16 ds;
    };
    struct {
        u8 dil, dih, hdi1, hdi2;
        u8 sil, sih, hsi1, hsi2;
        u8 bpl, bph, hbp1, hbp2;
        u8 _spl, _sph, _hsp1, _hsp2;
        u8 bl, bh, hb1, hb2;
        u8 dl, dh, hd1, hd2;
        u8 cl, ch, hc1, hc2;
        u8 al, ah, ha1, ha2;
    };
} regs;

// general purpose registers present in long mode
typedef struct {
    u64 r15, r14, r13, r12;
    u64 r11, r10, r9, r8;

    u64 rbp, rdi, rsi;
    u64 rdx, rcx, rbx, rax;
} gen_regs;

// specialized registers present in long mode
typedef struct {
    u64 rip, cs, rflags, rsp, ss;
} spec_regs;
