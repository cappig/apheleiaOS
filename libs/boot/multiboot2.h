#pragma once

#include <base/types.h>

// This header file is a modified version of the referance
// implementation featured in the multiboot2 specification:
// https://www.gnu.org/software/grub/manual/multiboot2/multiboot.html
// Original is created by the FSF. and licenced under the MIT licence

#define MB_SEARCH_LEN   32768
#define MB_HEADER_ALIGN 8

#define MB2_HEADER_MAGIC     0xe85250d6
#define MB2_BOOTLOADER_MAGIC 0x36d76289

#define MB_MOD_ALIGN  0x1000
#define MB_INFO_ALIGN 8

enum multiboot2_tag_type {
    MB_TT_END = 0,
    MB_TT_CMDLINE = 1,
    MB_TT_LOADER_NAME = 2,
    MB_TT_MODULE = 3,
    MB_TT_BASIC_MEMINFO = 4,
    MB_TT_BOOTDEV = 5,
    MB_TT_MMAP = 6,
    MB_TT_VBE = 7,
    MB_TT_FRAMEBUFFER = 8,
    MB_TT_ELF_SECTIONS = 9,
    MB_TT_APM = 10,
    MB_TT_EFI32 = 11,
    MB_TT_EFI64 = 12,
    MB_TT_SMBIOS = 13,
    MB_TT_ACPI_OLD = 14,
    MB_TT_ACPI_NEW = 15,
    MB_TT_NETWORK = 16,
    MB_TT_EFI_MMAP = 17,
    MB_TT_EFI_BS = 18,
    MB_TT_EFI32_IH = 19,
    MB_TT_EFI64_IH = 20,
    MB_TT_LOAD_BASE_ADDR = 21,
};

#define MB_HEADER_TAG_OPTIONAL 1

enum multiboot2_header_tag {
    MB_HT_END = 0,
    MB_HT_INFORMATION_REQUEST = 1,
    MB_HT_ADDRESS = 2,
    MB_HT_ENTRY_ADDRESS = 3,
    MB_HT_CONSOLE_FLAGS = 4,
    MB_HT_FRAMEBUFFER = 5,
    MB_HT_MODULE_ALIGN = 6,
    MB_HT_EFI_BS = 7,
    MB_HT_ENTRY_EFI32 = 8,
    MB_HT_ENTRY_EFI64 = 9,
    MB_HT_RELOCATABLE = 10,
};

#define MB_ARCHITECTURE_I386   0
#define MB_ARCHITECTURE_MIPS32 4

#define MB_LOAD_PREFERENCE_NONE 0
#define MB_LOAD_PREFERENCE_LOW  1
#define MB_LOAD_PREFERENCE_HIGH 2

#define MB_CONSOLE_REQUIRED      1
#define MB_CONSOLE_EGA_SUPPORTED 2

typedef struct {
    u32 magic;
    u32 architecture;
    u32 header_length;

    // The above fields plus this one must equal 0 mod 2^32
    u32 checksum;
} mb2_header;

typedef struct {
    u16 type;
    u16 flags;
    u32 size;
} mb2_header_tag;

typedef struct {
    u16 type;
    u16 flags;
    u32 size;
    u32 requests[];
} mb2_info_request_htag;

typedef struct {
    u16 type;
    u16 flags;
    u32 size;
    u32 header_addr;
    u32 load_addr;
    u32 load_end_addr;
    u32 bss_end_addr;
} mb2_addr_htag;

typedef struct {
    u16 type;
    u16 flags;
    u32 size;
    u32 entry_addr;
} mb2_entry_addr_htag;

typedef struct {
    u16 type;
    u16 flags;
    u32 size;
    u32 console_flags;
} mb2_console_flags_htag;

typedef struct {
    u16 type;
    u16 flags;
    u32 size;
    u32 width;
    u32 height;
    u32 depth;
} mb2_framebuffer_htag;

typedef struct {
    u16 type;
    u16 flags;
    u32 size;
} mb2_module_align_htag;

typedef struct {
    u16 type;
    u16 flags;
    u32 size;
    u32 min_addr;
    u32 max_addr;
    u32 align;
    u32 preference;
} mb2_relocatable_htag;

typedef struct {
    u8 red;
    u8 green;
    u8 blue;
} mb2_color;

#define MB2_MMAP_AVAILABLE 1
#define MB2_MMAP_RESERVED  2
#define MB2_MMAP_ACPI      3
#define MB2_MMAP_NVS       4
#define MB2_MMAP_BADRAM    5

typedef struct {
    u64 addr;
    u64 len;
    u32 type;
    u32 zero;
} mb2_mmap_entry;

typedef struct {
    u32 type;
    u32 size;
} mb2_tag;

typedef struct {
    u32 type;
    u32 size;
    char string[];
} mb2_string_tag;

typedef struct {
    u32 type;
    u32 size;
    u32 mod_start;
    u32 mod_end;
    char cmdline[0];
} mb2_module_tag;

typedef struct {
    u32 type;
    u32 size;
    u32 mem_lower;
    u32 mem_upper;
} mb2_basic_meminfo_tag;

typedef struct {
    u32 type;
    u32 size;
    u32 biosdev;
    u32 slice;
    u32 part;
} mb2_bootdev_tag;

typedef struct {
    u32 type;
    u32 size;
    u32 entry_size;
    u32 entry_version;
    mb2_mmap_entry entries[];
} mb2_mmap_tag;

typedef struct {
    u8 external_spec[512];
} mb2_vbe_info;

typedef struct {
    u8 external_specification[256];
} mb2_vbe_mode_info;

typedef struct {
    u32 type;
    u32 size;

    u16 vbe_mode;
    u16 vbe_interface_seg;
    u16 vbe_interface_off;
    u16 vbe_interface_len;

    mb2_vbe_info vbe_control_info;
    mb2_vbe_mode_info vbe_mode_info;
} mb2_vbe_tag;

#define MB2_FB_INDEXED  0
#define MB2_FB_RGB      1
#define MB2_FB_EGA_TEXT 2

typedef struct {
    u32 type;
    u32 size;

    u64 framebuffer_addr;
    u32 framebuffer_pitch;
    u32 framebuffer_width;
    u32 framebuffer_height;
    u8 framebuffer_bpp;
    u8 framebuffer_type;
    u16 reserved;
} mb2_framebuffer_common;

typedef struct {
    mb2_framebuffer_common common;

    union {
        struct {
            u16 palette_num_colors;
            mb2_color palette[];
        };
        struct {
            u8 red_field_position;
            u8 red_mask_size;
            u8 green_field_position;
            u8 green_mask_size;
            u8 blue_field_position;
            u8 blue_mask_size;
        };
    };
} mb2_framebuffer_tag;

typedef struct {
    u32 type;
    u32 size;
    u32 num;
    u32 entsize;
    u32 shndx;
    u8 sections[];
} mb2_elf_sections_tag;

typedef struct {
    u32 type;
    u32 size;
    u16 version;
    u16 cseg;
    u32 offset;
    u16 cseg_16;
    u16 dseg;
    u16 flags;
    u16 cseg_len;
    u16 cseg_16_len;
    u16 dseg_len;
} mb2_apm_tag;

typedef struct {
    u32 type;
    u32 size;
    u32 pointer;
} mb2_efi32_tag;

typedef struct {
    u32 type;
    u32 size;
    u64 pointer;
} mb2_efi64_tag;

typedef struct {
    u32 type;
    u32 size;
    u8 major;
    u8 minor;
    u8 _reserved[6];
    u8 tables[];
} mb2_smbios_tag;

typedef struct {
    u32 type;
    u32 size;
    u8 rsdp[];
} mb2_old_acpi_tag;

typedef struct {
    u32 type;
    u32 size;
    u8 rsdp[];
} mb2_new_acpi_tag;

typedef struct {
    u32 type;
    u32 size;
    u8 dhcpack[];
} mb2_network_tag;

typedef struct {
    u32 type;
    u32 size;
    u32 descr_size;
    u32 descr_vers;
    u8 efi_mmap[];
} mb2_efi_mmap_tag;

typedef struct {
    u32 type;
    u32 size;
    u32 pointer;
} mb2_efi32_ih_tag;

typedef struct {
    u32 type;
    u32 size;
    u64 pointer;
} mb2_efi64_ih_tag;

typedef struct {
    u32 type;
    u32 size;
    u32 load_base_addr;
} mb2_load_base_addr_tag;
