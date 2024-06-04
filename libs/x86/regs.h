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

typedef struct PACKED {
    u16 gs;
    u16 fs;
    u16 es;
    u16 ds;
} seg_regs;

// General purpose registers present in long mode
typedef struct PACKED {
    u64 r15;
    u64 r14;
    u64 r13;
    u64 r12;
    u64 r11;
    u64 r10;
    u64 r9;
    u64 r8;
    u64 rbp;
    u64 rdi;
    u64 rsi;
    u64 rdx;
    u64 rcx;
    u64 rbx;
    u64 rax;
} gen_regs;

// Specialized registers present in long mode
typedef struct PACKED {
    u64 rip;
    u64 cs;
    u64 rflags;
    u64 rsp;
    u64 ss;
} spec_regs;
