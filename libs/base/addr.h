#pragma once

#include "types.h"

#define PROTECTED_MODE_TOP 0x100000000UL

#define HIGHER_HALF_BASE 0xffffffff80000000ULL
#define ID_MAP_OFFSET    0xffff888000000000ULL

#define ID_MAPPED_VADDR(paddr) ((u64)(uptr)paddr + ID_MAP_OFFSET)
#define ID_MAPPED_PADDR(vaddr) ((u64)(uptr)vaddr - ID_MAP_OFFSET)

#define SEG_TO_PTR(segment, offset) (((u32)(segment) << 4) + (u32)(offset))

#define SEGMENT(address) (u16)(((int)(uptr)address & 0xffff0) >> 4)
#define OFFSET(address)  (u16)((int)(uptr)address & 0x0000f)

typedef struct {
    u8 off;
    u8 seg;
} u16_addr;

typedef struct {
    u16 off;
    u16 seg;
} u32_addr;

typedef struct {
    u32 off;
    u32 seg;
} u64_addr;
