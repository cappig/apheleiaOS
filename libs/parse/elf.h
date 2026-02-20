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
} elf_header_t;

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

    u32 entry;

    u32 phoff;
    u32 shoff;

    u32 flags;
    u16 hdr_size;

    u16 phent_size;
    u16 ph_num;

    u16 shdr_size;
    u16 sh_num;

    u16 shstrndx;
} elf32_header_t;

typedef struct PACKED {
    u32 type;
    u32 offset;
    u32 vaddr;
    u32 paddr;
    u32 file_size;
    u32 mem_size;
    u32 flags;
    u32 align;
} elf32_prog_header_t;

enum elf_arch {
    EARCH_32 = 1,
    EARCH_64 = 2,
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
enum elf_machine {
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
} elf_prog_header_t;

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
} elf_sect_header_t;

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
} elf_symbol_t;

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
    VALID_ELF = 0,
    INVALID_ELF = 1,
    WRONG_ENDIAN_ELF = 2,
} elf_validity_t;

typedef struct {
    u64 base;
    u64 top;
    u64 alignment;
} elf_attributes_t;

typedef struct {
    const u8 *blob;
    size_t blob_size;
    u8 elf_class;
    size_t shoff;
    size_t shent_size;
    size_t sh_num;
    size_t shstrndx;
} elf_view_t;

typedef struct {
    u32 name;
    u32 type;
    u64 flags;
    u64 addr;
    size_t offset;
    size_t size;
    u32 link;
    u32 info;
    u64 align;
    size_t ent_size;
} elf_section_view_t;

typedef struct {
    u32 name;
    u16 shndx;
    u64 value;
} elf_symbol_view_t;


bool elf_is_executable(elf_header_t *eheader);
elf_validity_t elf_verify(elf_header_t *header);

// u64 elf_to_page_flags(u32 elf_flags);
u64 elf_to_mmap_prot(u32 elf_flags);

bool elf_parse_header(elf_attributes_t *attribs, elf_header_t *header);

elf_sect_header_t *elf_locate_section(elf_header_t *header, const char *name);
elf_symbol_t *elf_locate_symbol(
    elf_symbol_t *symtab,
    size_t symtab_size,
    char *strtab,
    const char *name
);

bool elf_view_init(elf_view_t *view, const void *blob, size_t blob_size);
bool elf_view_read_section(
    const elf_view_t *view,
    size_t idx,
    elf_section_view_t *out
);
bool elf_view_section_data_ok(
    const elf_view_t *view,
    const elf_section_view_t *section
);
bool elf_view_find_section(
    const elf_view_t *view,
    const char *name,
    elf_section_view_t *out_section
);
bool elf_view_read_symbol(
    const elf_view_t *view,
    const u8 *entry,
    size_t ent_size,
    elf_symbol_view_t *out
);
size_t elf_view_min_symbol_size(const elf_view_t *view);
