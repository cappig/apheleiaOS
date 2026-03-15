#include <base/attributes.h>
#include <base/macros.h>
#include <base/types.h>
#include <fs/ext2.h>
#include <lib/boot.h>
#include <parse/elf.h>
#include <x86/asm.h>
#include <x86/boot.h>
#include <x86/paging64.h>

#include "efi.h"
#include "util.h"

static EFI_SYSTEM_TABLE *g_st = NULL;
static EFI_BOOT_SERVICES *g_bs = NULL;
static page_t *g_lvl4 = NULL;
static char boot_log_buf[BOOT_LOG_CAP];
static size_t boot_log_len = 0;

static const EFI_GUID loaded_image_guid = {
    0x5b1b31a1,
    0x9562,
    0x11d2,
    {0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}
};
static const EFI_GUID simple_fs_guid = {
    0x0964e5b22,
    0x6459,
    0x11d2,
    {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}
};
static const EFI_GUID file_info_guid = {
    0x09576e92,
    0x6d3f,
    0x11d2,
    {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}
};
static const EFI_GUID gop_guid = {
    0x9042a9de,
    0x23dc,
    0x4a38,
    {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}
};
static const EFI_GUID acpi_guid = {
    0xeb9d2d30,
    0x2d88,
    0x11d3,
    {0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}
};
static const EFI_GUID acpi2_guid = {
    0x8868e871,
    0xe4f1,
    0x11d3,
    {0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81}
};

static void _boot_log_append(const CHAR16 *s) {
    if (!s) {
        return;
    }

    while (*s) {
        if (boot_log_len >= BOOT_LOG_CAP) {
            return;
        }

        CHAR16 c = *s++;
        boot_log_buf[boot_log_len++] = (c <= 0x7f) ? (char)c : '?';
    }
}

static void _uefi_puts(const CHAR16 *s) {
    if (!s) {
        return;
    }

    _boot_log_append(s);

    if (!g_st || !g_st->ConOut || !g_st->ConOut->OutputString) {
        return;
    }

    g_st->ConOut->OutputString(g_st->ConOut, s);
}

static void _uefi_put_hex64(u64 value) {
    static const CHAR16 hex[] = L"0123456789ABCDEF";
    CHAR16 out[19];

    out[0] = L'0';
    out[1] = L'x';

    for (int i = 0; i < 16; i++) {
        int shift = (15 - i) * 4;
        out[2 + i] = hex[(value >> shift) & 0x0f];
    }

    out[18] = 0;

    _uefi_puts(out);
}

static EFI_STATUS _uefi_fail(const CHAR16 *msg, EFI_STATUS status) {
    if (msg) {
        _uefi_puts(msg);
    }

    _uefi_put_hex64(status);
    _uefi_puts((const CHAR16 *)L"\r\n");

    if (g_bs && g_bs->Stall) {
        typedef EFI_STATUS(EFIAPI * efi_stall_t)(UINTN);
        ((efi_stall_t)g_bs->Stall)(3000000);
    }

    return status;
}

// a bit of repeated code here... TODO: can we do anything about this?
static bool _ascii_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static char _ascii_to_lower(char c) {
    if (c >= 'A' && c <= 'Z') {
        return (char)(c - 'A' + 'a');
    }

    return c;
}

static bool _token_ieq(const char *begin, const char *end, const char *value) {
    if (!begin || !end || !value || end < begin) {
        return false;
    }

    const char *p = begin;
    const char *q = value;

    while (p < end && *q) {
        if (_ascii_to_lower(*p) != _ascii_to_lower(*q)) {
            return false;
        }

        p++;
        q++;
    }

    return (p == end) && (*q == '\0');
}

static bool
_parse_bool_token(const char *begin, const char *end, bool *value_out) {
    if (!begin || !end || !value_out || end < begin) {
        return false;
    }

    while (begin < end && _ascii_is_space(*begin)) {
        begin++;
    }

    while (end > begin && _ascii_is_space(*(end - 1))) {
        end--;
    }

    if (_token_ieq(begin, end, "1") || _token_ieq(begin, end, "true")) {
        *value_out = true;
        return true;
    }

    if (_token_ieq(begin, end, "0") || _token_ieq(begin, end, "false")) {
        *value_out = false;
        return true;
    }

    return false;
}

static void _parse_loader_conf(EFI_HANDLE image, boot_info_t *info) {
    if (!image || !info || !g_bs) {
        return;
    }

    void *config_data = NULL;
    UINTN config_size = 0;
    EFI_STATUS status = EFI_NOT_FOUND;
    const CHAR16 *candidates[] = {
        (const CHAR16 *)L"\\boot\\loader.conf",
        (const CHAR16 *)L"\\loader.conf",
    };

    for (size_t i = 0; i < ARRAY_LEN(candidates); i++) {
        config_data = NULL;
        config_size = 0;
        status = uefi_load_file_from_boot_volume(
            g_bs,
            image,
            &loaded_image_guid,
            &simple_fs_guid,
            &file_info_guid,
            candidates[i],
            &config_data,
            &config_size
        );

        if (!efi_error(status) && config_data && config_size) {
            break;
        }

        if (config_data) {
            g_bs->FreePool(config_data);
            config_data = NULL;
        }
    }

    if (efi_error(status) || !config_data || !config_size) {
        return;
    }

    const char *text = (const char *)config_data;
    size_t cursor = 0;

    while (cursor < config_size) {
        const char *line_begin = text + cursor;

        while (cursor < config_size && text[cursor] != '\n') {
            cursor++;
        }

        const char *line_end = text + cursor;
        if (cursor < config_size && text[cursor] == '\n') {
            cursor++;
        }

        const char *begin = line_begin;
        const char *end = line_end;

        while (begin < end && _ascii_is_space(*begin)) {
            begin++;
        }

        if (begin >= end || *begin == '#') {
            continue;
        }

        const char *comment = begin;
        while (comment < end && *comment != '#') {
            comment++;
        }
        end = comment;

        while (end > begin && _ascii_is_space(*(end - 1))) {
            end--;
        }

        if (begin >= end) {
            continue;
        }

        const char *eq = begin;
        while (eq < end && *eq != '=') {
            eq++;
        }

        if (eq >= end) {
            continue;
        }

        const char *key_begin = begin;
        const char *key_end = eq;
        const char *value_begin = eq + 1;
        const char *value_end = end;

        while (key_begin < key_end && _ascii_is_space(*key_begin)) {
            key_begin++;
        }
        while (key_end > key_begin && _ascii_is_space(*(key_end - 1))) {
            key_end--;
        }

        while (value_begin < value_end && _ascii_is_space(*value_begin)) {
            value_begin++;
        }
        while (value_end > value_begin && _ascii_is_space(*(value_end - 1))) {
            value_end--;
        }

        if (key_begin >= key_end || value_begin >= value_end) {
            continue;
        }

        if (_token_ieq(key_begin, key_end, "stage_rootfs") ||
            _token_ieq(key_begin, key_end, "stage_roootfs")) {
            bool enabled = info->args.stage_rootfs != 0;

            if (_parse_bool_token(value_begin, value_end, &enabled)) {
                info->args.stage_rootfs = enabled ? 1 : 0;
            }
        }
    }

    g_bs->FreePool(config_data);
}

static EFI_STATUS _alloc_pages_low(UINTN pages, EFI_PHYSICAL_ADDRESS *addr) {
    if (!addr) {
        return EFI_INVALID_PARAM;
    }

    *addr = 0xffffffffULL;
    return g_bs->AllocatePages(AllocateMaxAddress, EfiLoaderData, pages, addr);
}

static EFI_STATUS
_alloc_pages_below(UINTN pages, EFI_PHYSICAL_ADDRESS max, EFI_PHYSICAL_ADDRESS *addr) {
    if (!addr) {
        return EFI_INVALID_PARAM;
    }

    *addr = max;
    return g_bs->AllocatePages(AllocateMaxAddress, EfiLoaderData, pages, addr);
}

static EFI_STATUS _alloc_table(page_t **table) {
    if (!table) {
        return EFI_INVALID_PARAM;
    }

    EFI_PHYSICAL_ADDRESS addr = 0;
    EFI_STATUS status = _alloc_pages_low(1, &addr);

    if (efi_error(status)) {
        return status;
    }

    uefi_mem_zero((void *)(uintptr_t)addr, EFI_PAGE_SIZE);
    *table = (page_t *)(uintptr_t)addr;
    return EFI_SUCCESS;
}

static EFI_STATUS _walk_table(page_t *table, size_t index, page_t **next) {
    if (!table || !next) {
        return EFI_INVALID_PARAM;
    }

    if (table[index] & PT_PRESENT) {
        *next = (page_t *)(uintptr_t)page_get_paddr(&table[index]);
        return EFI_SUCCESS;
    }

    page_t *child = NULL;
    EFI_STATUS status = _alloc_table(&child);

    if (efi_error(status)) {
        return status;
    }

    table[index] = ((u64)(uintptr_t)child & ADDR_MASK) | PT_PRESENT | PT_WRITE;
    *next = child;
    return EFI_SUCCESS;
}

static EFI_STATUS _map_page_4k(u64 vaddr, u64 paddr, u64 flags) {
    page_t *lvl3 = NULL;
    page_t *lvl2 = NULL;
    page_t *lvl1 = NULL;

    EFI_STATUS status = _walk_table(g_lvl4, GET_LVL4_INDEX(vaddr), &lvl3);
    if (efi_error(status)) {
        return status;
    }

    status = _walk_table(lvl3, GET_LVL3_INDEX(vaddr), &lvl2);
    if (efi_error(status)) {
        return status;
    }

    status = _walk_table(lvl2, GET_LVL2_INDEX(vaddr), &lvl1);
    if (efi_error(status)) {
        return status;
    }

    size_t index = GET_LVL1_INDEX(vaddr);
    lvl1[index] = (paddr & ADDR_MASK) | (flags & FLAGS_MASK) | PT_PRESENT;
    return EFI_SUCCESS;
}

static EFI_STATUS _map_page_2m(u64 vaddr, u64 paddr, u64 flags) {
    page_t *lvl3 = NULL;
    page_t *lvl2 = NULL;

    EFI_STATUS status = _walk_table(g_lvl4, GET_LVL4_INDEX(vaddr), &lvl3);
    if (efi_error(status)) {
        return status;
    }

    status = _walk_table(lvl3, GET_LVL3_INDEX(vaddr), &lvl2);
    if (efi_error(status)) {
        return status;
    }

    size_t index = GET_LVL2_INDEX(vaddr);
    u64 pat_huge = flags & PT_PAT_HUGE;
    lvl2[index] = 
        (paddr & ADDR_MASK) | (flags & FLAGS_MASK) | pat_huge | PT_PRESENT | PT_HUGE;

    return EFI_SUCCESS;
}

static EFI_STATUS _map_region_4k(u64 vaddr, u64 paddr, u64 size, u64 flags) {
    u64 pages = DIV_ROUND_UP(size, PAGE_4KIB);

    for (u64 i = 0; i < pages; i++) {
        EFI_STATUS status =
            _map_page_4k(vaddr + i * PAGE_4KIB, paddr + i * PAGE_4KIB, flags);

        if (efi_error(status)) {
            return status;
        }
    }

    return EFI_SUCCESS;
}

static EFI_STATUS _map_identity_and_linear(void) {
    for (u64 addr = 0; addr < PROTECTED_MODE_TOP; addr += PAGE_2MIB) {
        EFI_STATUS status = _map_page_2m(addr, addr, PT_WRITE);

        if (efi_error(status)) {
            return status;
        }

        status = _map_page_2m(addr + LINEAR_MAP_OFFSET_64, addr, PT_WRITE);
        if (efi_error(status)) {
            return status;
        }
    }

    return EFI_SUCCESS;
}

static EFI_STATUS
_map_region_identity_and_linear(u64 paddr, u64 size, u64 flags) {
    if (!size) {
        return EFI_SUCCESS;
    }

    u64 base = ALIGN_DOWN(paddr, PAGE_4KIB);
    u64 end = ALIGN(paddr + size, PAGE_4KIB);

    if (end <= base) {
        return EFI_SUCCESS;
    }

    EFI_STATUS status = _map_region_4k(base, base, end - base, flags);
    if (efi_error(status)) {
        return status;
    }

    return _map_region_4k(base + LINEAR_MAP_OFFSET_64, base, end - base, flags);
}

static EFI_STATUS _map_loader_runtime(EFI_HANDLE image) {
    if (!image || !g_bs) {
        return EFI_INVALID_PARAM;
    }

    EFI_LOADED_IMAGE_PROTOCOL *loaded_image = NULL;
    EFI_STATUS status = g_bs->HandleProtocol(
        image, (EFI_GUID *)(uintptr_t)&loaded_image_guid, (void **)&loaded_image
    );

    if (efi_error(status) || !loaded_image) {
        return status;
    }

    u64 image_base = (u64)(uintptr_t)loaded_image->ImageBase;
    u64 image_size = loaded_image->ImageSize;

    if (image_base && image_size) {
        status =
            _map_region_identity_and_linear(image_base, image_size, PT_WRITE);

        if (efi_error(status)) {
            return status;
        }
    }

    u64 rsp = 0;
    asm volatile("mov %%rsp, %0" : "=r"(rsp));

    const u64 stack_window = 128 * 1024;
    u64 stack_base = (rsp > stack_window / 2) ? (rsp - stack_window / 2) : 0;

    status =
        _map_region_identity_and_linear(stack_base, stack_window, PT_WRITE);

    if (efi_error(status)) {
        return status;
    }

    return EFI_SUCCESS;
}

static EFI_STATUS _map_framebuffer_linear(const boot_info_t *info) {
    if (
        !info ||
        info->video.mode != VIDEO_GRAPHICS ||
        !info->video.framebuffer ||
        !info->video.width ||
        !info->video.height ||
        !info->video.bytes_per_pixel
    ) {
        return EFI_SUCCESS;
    }

    u64 pitch = info->video.bytes_per_line;
    if (!pitch) {
        pitch = (u64)info->video.width * info->video.bytes_per_pixel;
    }

    u64 fb_size = pitch * info->video.height;
    if (!fb_size) {
        return EFI_SUCCESS;
    }

    u64 fb_start = info->video.framebuffer;
    u64 fb_end = fb_start + fb_size;

    // Remap the 2MiB-aligned region covering the framebuffer as write-combining
    u64 base_2m = ALIGN_DOWN(fb_start, PAGE_2MIB);
    u64 end_2m = ALIGN(fb_end, PAGE_2MIB);

    for (u64 addr = base_2m; addr < end_2m; addr += PAGE_2MIB) {
        if (addr < PROTECTED_MODE_TOP) {
            EFI_STATUS s = _map_page_2m(addr, addr, PT_WRITE | PT_PAT_HUGE);
            if (efi_error(s)) {
                return s;
            }

            s = _map_page_2m(
                addr + LINEAR_MAP_OFFSET_64, addr, PT_WRITE | PT_PAT_HUGE
            );

            if (efi_error(s)) {
                return s;
            }
        } else {
            // Above 4 GiB — no pre-existing mapping; use 4KB WC pages
            u64 chunk_base = ALIGN_DOWN(addr, PAGE_4KIB);
            u64 chunk_end = addr + PAGE_2MIB;

            if (chunk_end > end_2m) {
                chunk_end = end_2m;
            }

            EFI_STATUS s = _map_region_4k(
                chunk_base + LINEAR_MAP_OFFSET_64,
                chunk_base,
                chunk_end - chunk_base,
                PT_WRITE | PT_PAT_4K
            );

            if (efi_error(s)) {
                return s;
            }
        }
    }

    return EFI_SUCCESS;
}


static EFI_STATUS
_load_kernel_elf(void *file_data, UINTN file_size, u64 *entry) {
    if (!file_data || !entry || file_size < sizeof(elf_header_t)) {
        return EFI_LOAD_ERROR;
    }

    elf_header_t *header = (elf_header_t *)file_data;

    if (header->magic != ELF_MAGIC || header->version != ELF_VERSION) {
        return EFI_LOAD_ERROR;
    }

    if (header->endianness != EEND_LITTLE) {
        return EFI_LOAD_ERROR;
    }

    if (header->arch != EARCH_64 || header->machine != EM_X86_64) {
        return EFI_LOAD_ERROR;
    }

    if (!header->phoff || !header->ph_num || !header->phent_size) {
        return EFI_LOAD_ERROR;
    }

    u64 phdr_end =
        header->phoff + (u64)header->ph_num * (u64)header->phent_size;

    if (phdr_end > file_size) {
        return EFI_LOAD_ERROR;
    }

    for (size_t i = 0; i < header->ph_num; i++) {
        size_t offset = header->phoff + i * header->phent_size;
        elf_prog_header_t *ph =
            (elf_prog_header_t *)((u8 *)file_data + offset);

        if (ph->type != PT_LOAD) {
            continue;
        }

        if (!ph->mem_size) {
            continue;
        }

        u64 seg_end = ph->offset + ph->file_size;
        if (seg_end > file_size) {
            return EFI_LOAD_ERROR;
        }

        UINTN pages = (UINTN)DIV_ROUND_UP(ph->mem_size, PAGE_4KIB);
        EFI_PHYSICAL_ADDRESS paddr = 0;

        EFI_STATUS status = _alloc_pages_low(pages, &paddr);

        if (efi_error(status)) {
            return status;
        }

        uefi_mem_zero((void *)(uintptr_t)paddr, pages * EFI_PAGE_SIZE);
        uefi_mem_copy(
            (void *)(uintptr_t)paddr,
            (u8 *)file_data + ph->offset,
            ph->file_size
        );

        u64 flags = 0;
        if (ph->flags & PF_W) {
            flags |= PT_WRITE;
        }

        status = _map_region_4k(ph->vaddr, paddr, ph->mem_size, flags);
        if (efi_error(status)) {
            return status;
        }
    }

    *entry = header->entry;
    return EFI_SUCCESS;
}

static void _setup_default_args(boot_info_t *info) {
    if (!info) {
        return;
    }

    uefi_mem_zero(&info->args, sizeof(info->args));

    info->args.debug = DEBUG_ALL;
    info->args.stage_rootfs = BOOT_DEFAULT_STAGE_ROOTFS;
    info->args.video = BOOT_DEFAULT_VIDEO;
    info->args.vesa_width = (u16)BOOT_DEFAULT_VESA_WIDTH;
    info->args.vesa_height = (u16)BOOT_DEFAULT_VESA_HEIGHT;
    info->args.vesa_bpp = BOOT_DEFAULT_VESA_BPP;

    uefi_str_copy(
        info->args.console,
        sizeof(info->args.console),
        "/dev/ttyS0,/dev/console"
    );

    uefi_str_copy(info->args.font, sizeof(info->args.font), BOOT_DEFAULT_FONT);
}

static void _set_root_hint_defaults(boot_root_hint_t *hint) {
    if (!hint) {
        return;
    }

    uefi_mem_zero(hint, sizeof(*hint));
    hint->valid = 1;
    hint->media = BOOT_MEDIA_UNKNOWN;
    hint->transport = BOOT_TRANSPORT_UNKNOWN;
    hint->part_style = BOOT_PARTSTYLE_UNKNOWN;
    hint->part_index = 0;
    hint->bios_drive = 0;
}

static void _populate_root_hint_from_staged(
    boot_info_t *info,
    const void *rootfs_file,
    UINTN rootfs_size
) {
    if (!info || !rootfs_file || rootfs_size < 1024 + sizeof(ext2_superblock_t)) {
        return;
    }

    _set_root_hint_defaults(&info->boot_root_hint);

    const u8 *bytes = (const u8 *)rootfs_file;
    const ext2_superblock_t *sb =
        (const ext2_superblock_t *)(const void *)(bytes + 1024);

    if (sb->signature != EXT2_SIGNATURE) {
        return;
    }

    info->boot_root_hint.rootfs_uuid_valid = 1;
    uefi_mem_copy(
        info->boot_root_hint.rootfs_uuid,
        sb->fs_id,
        sizeof(info->boot_root_hint.rootfs_uuid)
    );
}

static void _stage_rootfs_from_esp(EFI_HANDLE image, boot_info_t *info) {
    if (!image || !info || !g_bs) {
        return;
    }

    void *rootfs_file = NULL;
    UINTN rootfs_size = 0;
    EFI_STATUS status = EFI_NOT_FOUND;
    const CHAR16 *candidates[] = {
        (const CHAR16 *)L"\\boot\\rootfs.img",
        (const CHAR16 *)L"\\rootfs.img",
        (const CHAR16 *)L"\\boot\\rootfs.ext2",
        (const CHAR16 *)L"\\rootfs.ext2",
    };

    for (size_t i = 0; i < ARRAY_LEN(candidates); i++) {
        rootfs_file = NULL;
        rootfs_size = 0;
        status = uefi_load_file_from_boot_volume(
            g_bs,
            image,
            &loaded_image_guid,
            &simple_fs_guid,
            &file_info_guid,
            candidates[i],
            &rootfs_file,
            &rootfs_size
        );

        if (!efi_error(status) && rootfs_file && rootfs_size) {
            break;
        }
    }

    if (efi_error(status) || !rootfs_file || !rootfs_size) {
        return;
    }

    UINTN pages = (UINTN)DIV_ROUND_UP(rootfs_size, PAGE_4KIB);
    EFI_PHYSICAL_ADDRESS rootfs_phys = 0;

    status = _alloc_pages_low(pages, &rootfs_phys);

    if (!efi_error(status)) {
        uefi_mem_zero((void *)(uintptr_t)rootfs_phys, pages * EFI_PAGE_SIZE);
        uefi_mem_copy((void *)(uintptr_t)rootfs_phys, rootfs_file, rootfs_size);
        info->boot_rootfs_paddr = (u64)rootfs_phys;
        info->boot_rootfs_size = (u64)rootfs_size;
        _populate_root_hint_from_staged(info, rootfs_file, rootfs_size);
    }

    g_bs->FreePool(rootfs_file);
}

static NORETURN void
_jump_to_kernel(u64 entry, u64 stack_top, boot_info_t *info) {
    asm volatile(
        "cli\n\t"
        "cld\n\t"
        "mov %0, %%rsp\n\t"
        "xor %%rbp, %%rbp\n\t"
        "jmp *%1"
        :
        : "r"(stack_top), "r"(entry), "D"(info)
        : "memory"
    );

    __builtin_unreachable();
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *system_table) {
    g_st = system_table;
    g_bs = system_table ? system_table->BootServices : NULL;

    if (!g_bs || !g_st) {
        return EFI_LOAD_ERROR;
    }

    _uefi_puts((const CHAR16 *)L"UEFI boot starting\r\n");

    if (g_bs->SetWatchdogTimer) {
        g_bs->SetWatchdogTimer(0, 0, 0, NULL);
    }

    void *kernel_file = NULL;
    UINTN kernel_file_size = 0;
    EFI_STATUS status = uefi_load_file_from_boot_volume(
        g_bs,
        image,
        &loaded_image_guid,
        &simple_fs_guid,
        &file_info_guid,
        (const CHAR16 *)L"\\boot\\kernel64.elf",
        &kernel_file,
        &kernel_file_size
    );

    if (efi_error(status)) {
        _uefi_puts((const CHAR16 *)L"failed to open kernel64.elf\r\n");
        return status;
    }

    EFI_PHYSICAL_ADDRESS info_phys = 0;
    status = _alloc_pages_low(1, &info_phys);

    if (efi_error(status)) {
        return _uefi_fail(
            (const CHAR16 *)L"boot info allocation failed ", status
        );
    }

    boot_info_t *info = (boot_info_t *)(uintptr_t)info_phys;
    uefi_mem_zero(info, sizeof(*info));
    info->boot_log_paddr = (u64)(uintptr_t)boot_log_buf;
    info->boot_log_cap = BOOT_LOG_CAP;

    EFI_PHYSICAL_ADDRESS smp_trampoline = 0;
    status = _alloc_pages_below(1, 0x000FFFFFULL, &smp_trampoline);
    if (!efi_error(status)) {
        info->smp_trampoline_paddr = (u64)smp_trampoline;
    }

    _setup_default_args(info);
    _parse_loader_conf(image, info);

    uefi_detect_acpi(info, g_st, &acpi2_guid, &acpi_guid);
    uefi_detect_video(info, g_bs, &gop_guid);

    status = _alloc_table(&g_lvl4);
    if (efi_error(status)) {
        return _uefi_fail(
            (const CHAR16 *)L"page table allocation failed ", status
        );
    }

    status = _map_identity_and_linear();
    if (efi_error(status)) {
        return _uefi_fail(
            (const CHAR16 *)L"low memory map failed ", status
        );
    }

    status = _map_loader_runtime(image);
    if (efi_error(status)) {
        return _uefi_fail(
            (const CHAR16 *)L"loader runtime map failed ", status
        );
    }

    status = _map_framebuffer_linear(info);
    if (efi_error(status)) {
        return _uefi_fail(
            (const CHAR16 *)L"framebuffer map failed ", status
        );
    }

    u64 kernel_entry = 0;
    status = _load_kernel_elf(kernel_file, kernel_file_size, &kernel_entry);

    if (efi_error(status)) {
        _uefi_puts((const CHAR16 *)L"failed to load kernel ELF\r\n");
        return status;
    }

    UINTN stack_pages = DIV_ROUND_UP(KERNEL_STACK_SIZE, PAGE_4KIB);
    EFI_PHYSICAL_ADDRESS stack_phys = 0;
    status = _alloc_pages_low(stack_pages, &stack_phys);
    if (efi_error(status)) {
        return _uefi_fail(
            (const CHAR16 *)L"kernel stack allocation failed ", status
        );
    }

    if (info->args.stage_rootfs) {
        _stage_rootfs_from_esp(image, info);
    }

    u64 stack_top = stack_phys + KERNEL_STACK_SIZE + LINEAR_MAP_OFFSET_64;
    boot_info_t *info_virt =
        (boot_info_t *)(uintptr_t)(info_phys + LINEAR_MAP_OFFSET_64);

    void *map_buf = NULL;
    UINTN map_buf_size = 0;
    UINTN map_key = 0;

    for (int attempts = 0; attempts < 8; attempts++) {
        status = uefi_get_memory_map_and_key(
            g_bs, info, &map_buf, &map_buf_size, &map_key
        );

        if (efi_error(status)) {
            return _uefi_fail(
                (const CHAR16 *)L"memory map fetch failed ", status
            );
        }

        status = g_bs->ExitBootServices(image, map_key);
        if (!efi_error(status)) {
            break;
        }
    }

    if (efi_error(status)) {
        return _uefi_fail(
            (const CHAR16 *)L"ExitBootServices failed ", status
        );
    }

    info->boot_log_len = (u32)boot_log_len;
    pat_init();

    write_cr3((u64)(uintptr_t)g_lvl4);

    u64 cr0 = read_cr0();
    write_cr0(cr0 | CR0_WP);

    _jump_to_kernel(kernel_entry, stack_top, info_virt);
}
