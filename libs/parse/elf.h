#pragma once

#include <base/attributes.h>
#include <base/types.h>

// Structs representing 64 bit elf file headers
// https://en.wikipedia.org/wiki/Executable_and_Linkable_Format

// 0x4f 'E' 'L' 'F'
#define ELF_MAGIC 0x464c457f

#define ELF_VERSION 1

typedef struct PACKED {
    u32 magic;
    u8 arch;
    u8 endianness;
    u8 id_version;
    u8 abi;
    u8 abi_version;
    u8 _unused0[7];

    u16 type;
    u16 machine;
    u32 version;

    u64 entry;

    u64 phoff;
    u64 shoff;

    u32 flags;
    u16 hdr_size;

    u16 phent_size; // size of a program header table entry
    u16 ph_num; // number of entries in the program header table

    u16 shdr_size; // size of a section header table entry
    u16 sh_num; // number of entries in the section header table

    u16 shstrndx; // index of the section header table entry
} elf_header;

enum elf_arch {
    EARCH_X32 = 1,
    EARCH_X64 = 2,
};

enum elf_endianness {
    EEND_LITTLE = 1,
    EEND_BIG = 2,
};

// Both of these represent the SYSV ABI
enum elf_abi {
    EABI_SYSV = 0x00,
    EABI_LINUX = 0x03,
};

enum elf_type {
    ET_NONE = 0,
    ET_REL = 1,
    ET_EXEC = 2,
    ET_DYN = 3,
    ET_CORE = 4,
};

// Only the important ones
enum ELF_machine {
    EM_NONE = 0x00,
    EM_X86 = 0x03,
    EM_X86_64 = 0x3E,
    ET_RISC_V = 0xF3,
};

typedef struct PACKED {
    u32 type;
    u32 flags;

    u64 offset;

    u64 vaddr;
    u64 paddr;

    // size in bytes
    u64 file_size;
    u64 mem_size;

    u64 align;
} elf_prog_header;

enum elf_program_flags {
    PF_X = 0x1, // Executable
    PF_W = 0x2, // Writable
    PF_R = 0x4, // Readable
};

enum elf_program_type {
    PT_NULL = 0,
    PT_LOAD = 1,
    PT_DYNAMIC = 2,
    PT_INTERP = 3,
    PT_NOTE = 4,
    PT_SHLIB = 5, // reserved
    PT_PHDR = 6,
    PT_TLS = 7,
};

typedef struct PACKED {
    u32 name;
    u32 type;

    u64 flags;

    u64 addr;
    u64 offset;
    u64 size;

    u32 link;
    u32 info;

    u64 align;

    u64 ent_size;
} elf_sect_header;

enum elf_section_type {
    SHT_NULL = 0x00,
    SHT_PROGBITS = 0x01,
    SHT_SYMTAB = 0x02,
    SHT_STRTAB = 0x03,
    SHT_RELA = 0x04,
    SHT_HASH = 0x05,
    SHT_DYNAMIC = 0x06,
    SHT_NOTE = 0x07,
    SHT_NOBITS = 0x08,
    SHT_REL = 0x09,
    SHT_SHLIB = 0x0a, // reserved
    SHT_DYNSYM = 0x0b,
    SHT_INIT_ARRAY = 0x0e,
    SHT_FINI_ARRAY = 0x0f,
    SHT_PREINIT_ARRAY = 0x10,
    SHT_GROUP = 0x11,
    SHT_SYMTAB_SHNDX = 0x13,
    SHT_NUM = 0x13,
};

enum elf_section_flags {
    SHF_WRITE = 0x1,
    SHF_ALLOC = 0x2,
    SHF_EXECINSTR = 0x4,
    SHF_MERGE = 0x10,
    SHF_STRINGS = 0x20,
    SHF_INFO_LINK = 0x40,
    SHF_LINK_ORDER = 0x80,
    SHF_OS_NONCONFORMING = 0x100,
    SHF_GROUP = 0x200,
    SHF_TLS = 0x400,
};

#define ELF_SYMBOL_BIND(info) ((info) >> 4)
#define ELF_SYMBOL_TYPE(info) ((info) & 0x0F)

typedef struct PACKED {
    u32 name;
    u8 info;
    u8 other;
    u16 shndx; // section table index
    u64 value;
    u64 size;
} elf_symbol;

enum elf_symbol_bindig {
    STB_LOCAL = 0,
    STB_GLOBAL = 1,
    STB_WEAK = 2,
};

enum elf_symbol_type {
    STT_NOTYPE = 0,
    STT_OBJECT = 1,
    STT_FUNC = 2,
};

typedef enum {
    VALID_ELF = 1,
    INVALID_ELF = -1,
    INVALID_ELF64 = -2,
    WRONG_ENDIAN_ELF = -3,
} elf_validity;

typedef struct {
    u64 base;
    u64 top;
    u64 alignment;
} elf_attributes;


bool elf_is_executable(elf_header* eheader);
elf_validity elf_verify(elf_header* header);

u64 elf_to_page_flags(u32 elf_flags);
u64 elf_to_mmap_prot(u32 elf_flags);

bool elf_parse_header(elf_attributes* attribs, elf_header* header);

elf_sect_header* elf_locate_section(elf_header* header, const char* name);
elf_symbol* elf_locate_symbol(elf_symbol* symtab, usize symtab_size, char* strtab, const char* name);
