#include <base/attributes.h>
#include <base/macros.h>
#include <base/types.h>
#include <fs/ext2.h>
#include <lib/boot.h>
#include <log/log.h>
#include <parse/elf.h>
#include <stdbool.h>
#include <x86/asm.h>
#include <x86/boot.h>
#include <x86/paging64.h>

#include "efi.h"
#include "util.h"

#ifndef BOOT_LOG_COLOR
#define BOOT_LOG_COLOR true
#endif

typedef struct {
    EFI_SYSTEM_TABLE *st;
    EFI_BOOT_SERVICES *bs;
    page_t *lvl4;

    char log[BOOT_LOG_CAP];
    size_t log_len;
} uefi_state_t;

static uefi_state_t uefi = { 0 };

static const EFI_GUID loaded_image_guid = {
    0x5b1b31a1,
    0x9562,
    0x11d2,
    { 0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b },
};

static const EFI_GUID simple_fs_guid = {
    0x0964e5b22,
    0x6459,
    0x11d2,
    { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b },
};

static const EFI_GUID file_info_guid = {
    0x09576e92,
    0x6d3f,
    0x11d2,
    { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b },
};

static const EFI_GUID gop_guid = {
    0x9042a9de,
    0x23dc,
    0x4a38,
    { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a },
};

static const EFI_GUID acpi_guid = {
    0xeb9d2d30,
    0x2d88,
    0x11d3,
    { 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d },
};

static const EFI_GUID acpi2_guid = {
    0x8868e871,
    0xe4f1,
    0x11d3,
    { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 },
};

typedef struct {
    const char *begin;
    const char *end;
} span_t;

static char ascii16(CHAR16 c) {
    if (c <= 0x7f) {
        return (char)c;
    }

    return '?';
}

static size_t min_size(size_t value, size_t limit) {
    if (value < limit) {
        return value;
    }

    return limit;
}

static void log_append(const CHAR16 *s) {
    if (!s) {
        return;
    }

    while (*s) {
        if (uefi.log_len >= BOOT_LOG_CAP) {
            return;
        }

        uefi.log[uefi.log_len++] = ascii16(*s++);
    }
}

static void set_attr(UINTN fg, UINTN bg) {
    if (!uefi.st || !uefi.st->ConOut || !uefi.st->ConOut->SetAttribute) {
        return;
    }

    uefi.st->ConOut->SetAttribute(uefi.st->ConOut, fg | (bg << 4));
}

static void write_text(const CHAR16 *s, size_t len) {
    if (!uefi.st || !uefi.st->ConOut || !uefi.st->ConOut->OutputString || !s || !len) {
        return;
    }

    CHAR16 chunk[LOG_BUF_SIZE];
    while (len) {
        size_t count = min_size(len, LOG_BUF_SIZE - 1);

        for (size_t i = 0; i < count; i++) {
            chunk[i] = s[i];
        }

        chunk[count] = 0;
        uefi.st->ConOut->OutputString(uefi.st->ConOut, chunk);

        s += count;
        len -= count;
    }
}

static void apply_sgr(const unsigned int *codes, size_t count) {
    UINTN fg = 7;
    UINTN bg = 0;

    if (!count) {
        set_attr(fg, bg);
        return;
    }

    for (size_t i = 0; i < count; i++) {
        switch (codes[i]) {
        case 0:
            fg = 7;
            bg = 0;
            break;
        case 31:
            fg = 4;
            break;
        case 32:
            fg = 2;
            break;
        case 33:
            fg = 14;
            break;
        case 36:
            fg = 3;
            break;
        case 37:
            fg = 7;
            break;
        case 41:
            bg = 4;
            break;
        case 90:
            fg = 8;
            break;
        case 97:
            fg = 15;
            break;
        default:
            break;
        }
    }

    set_attr(fg, bg);
}

static bool parse_sgr(const CHAR16 **cursor) {
    const CHAR16 *p = *cursor;
    unsigned int codes[4];
    size_t count = 0;

    if (p[0] != 0x1b || p[1] != '[') {
        return false;
    }

    p += 2;

    while (*p && count < ARRAY_LEN(codes)) {
        unsigned int value = 0;
        bool have_digit = false;

        while (*p >= '0' && *p <= '9') {
            value = value * 10U + (unsigned int)(*p - '0');
            have_digit = true;
            p++;
        }

        if (have_digit) {
            codes[count++] = value;
        }

        if (*p == ';') {
            p++;
            continue;
        }

        break;
    }

    if (*p != 'm') {
        return false;
    }

    apply_sgr(codes, count);
    *cursor = p + 1;
    return true;
}

static void write_ansi(const CHAR16 *s) {
    if (!s) {
        return;
    }

    const CHAR16 *text = s;
    const CHAR16 *p = s;

    while (*p) {
        if (*p != 0x1b) {
            p++;
            continue;
        }

        write_text(text, (size_t)(p - text));

        const CHAR16 *after = p;
        if (parse_sgr(&after)) {
            p = after;
            text = p;
            continue;
        }

        p++;
        text = p;
    }

    write_text(text, (size_t)(p - text));
    set_attr(7, 0);
}

static void puts16(const CHAR16 *s) {
    if (!s) {
        return;
    }

    log_append(s);
    write_ansi(s);
}

static EFI_STATUS fail(const CHAR16 *msg, EFI_STATUS status) {
    char text[96];
    size_t i = 0;

    if (msg) {
        const CHAR16 *p = msg;
        while (i < sizeof(text) - 1 && *p) {
            text[i++] = ascii16(*p++);
        }
    }

    text[i] = '\0';
    log_error("%s%#llx", text, (unsigned long long)status);

    if (uefi.bs && uefi.bs->Stall) {
        typedef EFI_STATUS(EFIAPI * efi_stall_t)(UINTN);
        ((efi_stall_t)uefi.bs->Stall)(3000000);
    }

    return status;
}

static unsigned int log_opts(void) {
#if BOOT_LOG_COLOR
    return LOG_OPT_LOCATION | LOG_OPT_COLOR;
#else
    return LOG_OPT_LOCATION;
#endif
}

static void log_sink(const char *s) {
    CHAR16 out[LOG_BUF_SIZE];
    size_t i = 0;

    if (!s) {
        return;
    }

    while (i < LOG_BUF_SIZE - 1 && s[i]) {
        out[i] = (unsigned char)s[i];
        i++;
    }

    out[i] = 0;
    puts16(out);
}

static bool ascii_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static char ascii_lower(char c) {
    if (c >= 'A' && c <= 'Z') {
        return (char)(c - 'A' + 'a');
    }

    return c;
}

static span_t trim_span(span_t span) {
    while (span.begin < span.end && ascii_space(*span.begin)) {
        span.begin++;
    }

    while (span.end > span.begin && ascii_space(*(span.end - 1))) {
        span.end--;
    }

    return span;
}

static bool token_eq_ci(span_t span, const char *value) {
    if (!span.begin || !span.end || !value || span.end < span.begin) {
        return false;
    }

    const char *p = span.begin;
    const char *q = value;

    while (p < span.end && *q) {
        if (ascii_lower(*p) != ascii_lower(*q)) {
            return false;
        }

        p++;
        q++;
    }

    return (p == span.end) && (*q == '\0');
}

static bool parse_bool(span_t span, bool *value_out) {
    if (!value_out) {
        return false;
    }

    span = trim_span(span);

    if (token_eq_ci(span, "1") || token_eq_ci(span, "true")) {
        *value_out = true;
        return true;
    }

    if (token_eq_ci(span, "0") || token_eq_ci(span, "false")) {
        *value_out = false;
        return true;
    }

    return false;
}

static bool split_conf_line(span_t line, span_t *key, span_t *value) {
    line = trim_span(line);

    if (line.begin >= line.end || *line.begin == '#') {
        return false;
    }

    span_t body = line;
    const char *comment = body.begin;

    while (comment < body.end && *comment != '#') {
        comment++;
    }

    body.end = comment;

    body = trim_span(body);
    if (body.begin >= body.end) {
        return false;
    }

    const char *eq = body.begin;
    while (eq < body.end && *eq != '=') {
        eq++;
    }

    if (eq >= body.end) {
        return false;
    }

    *key = trim_span((span_t){ body.begin, eq });
    *value = trim_span((span_t){ eq + 1, body.end });

    return key->begin < key->end && value->begin < value->end;
}

static void load_conf(EFI_HANDLE image, boot_info_t *info) {
    if (!image || !info || !uefi.bs) {
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
        status = uefi_load_file(
            uefi.bs,
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
            uefi.bs->FreePool(config_data);
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

        span_t key;
        span_t value;
        if (!split_conf_line((span_t){ line_begin, line_end }, &key, &value)) {
            continue;
        }

        if (token_eq_ci(key, "stage_rootfs")) {
            bool enabled = info->args.stage_rootfs != 0;

            if (parse_bool(value, &enabled)) {
                if (enabled) {
                    info->args.stage_rootfs = 1;
                } else {
                    info->args.stage_rootfs = 0;
                }
            }
        }
    }

    uefi.bs->FreePool(config_data);
}

static EFI_STATUS alloc_low(UINTN pages, EFI_PHYSICAL_ADDRESS *addr) {
    if (!addr) {
        return EFI_INVALID_PARAM;
    }

    *addr = 0xffffffffULL;
    return uefi.bs->AllocatePages(AllocateMaxAddress, EfiLoaderData, pages, addr);
}

static EFI_STATUS alloc_below(UINTN pages, EFI_PHYSICAL_ADDRESS max, EFI_PHYSICAL_ADDRESS *addr) {
    if (!addr) {
        return EFI_INVALID_PARAM;
    }

    *addr = max;
    return uefi.bs->AllocatePages(AllocateMaxAddress, EfiLoaderData, pages, addr);
}

static EFI_STATUS new_table(page_t **table) {
    if (!table) {
        return EFI_INVALID_PARAM;
    }

    EFI_PHYSICAL_ADDRESS addr = 0;
    EFI_STATUS status = alloc_low(1, &addr);

    if (efi_error(status)) {
        return status;
    }

    uefi_mem_zero((void *)(uintptr_t)addr, EFI_PAGE_SIZE);
    *table = (page_t *)(uintptr_t)addr;
    return EFI_SUCCESS;
}

static EFI_STATUS walk_table(page_t *table, size_t index, page_t **next) {
    if (!table || !next) {
        return EFI_INVALID_PARAM;
    }

    if (table[index] & PT_PRESENT) {
        *next = (page_t *)(uintptr_t)page_get_paddr(&table[index]);
        return EFI_SUCCESS;
    }

    page_t *child = NULL;
    EFI_STATUS status = new_table(&child);

    if (efi_error(status)) {
        return status;
    }

    table[index] = ((u64)(uintptr_t)child & ADDR_MASK) | PT_PRESENT | PT_WRITE;
    *next = child;
    return EFI_SUCCESS;
}

static EFI_STATUS map_page_4k(u64 vaddr, u64 paddr, u64 flags) {
    page_t *lvl3 = NULL;
    page_t *lvl2 = NULL;
    page_t *lvl1 = NULL;

    EFI_STATUS status = walk_table(uefi.lvl4, GET_LVL4_INDEX(vaddr), &lvl3);
    if (efi_error(status)) {
        return status;
    }

    status = walk_table(lvl3, GET_LVL3_INDEX(vaddr), &lvl2);
    if (efi_error(status)) {
        return status;
    }

    status = walk_table(lvl2, GET_LVL2_INDEX(vaddr), &lvl1);
    if (efi_error(status)) {
        return status;
    }

    size_t index = GET_LVL1_INDEX(vaddr);
    lvl1[index] = (paddr & ADDR_MASK) | (flags & FLAGS_MASK) | PT_PRESENT;
    return EFI_SUCCESS;
}

static EFI_STATUS map_page_2m(u64 vaddr, u64 paddr, u64 flags) {
    page_t *lvl3 = NULL;
    page_t *lvl2 = NULL;

    EFI_STATUS status = walk_table(uefi.lvl4, GET_LVL4_INDEX(vaddr), &lvl3);
    if (efi_error(status)) {
        return status;
    }

    status = walk_table(lvl3, GET_LVL3_INDEX(vaddr), &lvl2);
    if (efi_error(status)) {
        return status;
    }

    size_t index = GET_LVL2_INDEX(vaddr);
    u64 pat_huge = flags & PT_PAT_HUGE;
    lvl2[index] = (paddr & ADDR_MASK) | (flags & FLAGS_MASK) | pat_huge | PT_PRESENT | PT_HUGE;

    return EFI_SUCCESS;
}

static EFI_STATUS map_region_4k(u64 vaddr, u64 paddr, u64 size, u64 flags) {
    u64 pages = DIV_ROUND_UP(size, PAGE_4KIB);

    for (u64 i = 0; i < pages; i++) {
        EFI_STATUS status = map_page_4k(vaddr + i * PAGE_4KIB, paddr + i * PAGE_4KIB, flags);

        if (efi_error(status)) {
            return status;
        }
    }

    return EFI_SUCCESS;
}

static EFI_STATUS map_lowmem(void) {
    for (u64 addr = 0; addr < PROTECTED_MODE_TOP; addr += PAGE_2MIB) {
        EFI_STATUS status = map_page_2m(addr, addr, PT_WRITE);

        if (efi_error(status)) {
            return status;
        }

        status = map_page_2m(addr + LINEAR_MAP_OFFSET_64, addr, PT_WRITE);
        if (efi_error(status)) {
            return status;
        }
    }

    return EFI_SUCCESS;
}

static EFI_STATUS map_boot(u64 paddr, u64 size, u64 flags) {
    if (!size) {
        return EFI_SUCCESS;
    }

    u64 base = ALIGN_DOWN(paddr, PAGE_4KIB);
    u64 end = ALIGN(paddr + size, PAGE_4KIB);

    if (end <= base) {
        return EFI_SUCCESS;
    }

    EFI_STATUS status = map_region_4k(base, base, end - base, flags);
    if (efi_error(status)) {
        return status;
    }

    return map_region_4k(base + LINEAR_MAP_OFFSET_64, base, end - base, flags);
}

static EFI_STATUS map_loader(EFI_HANDLE image) {
    if (!image || !uefi.bs) {
        return EFI_INVALID_PARAM;
    }

    EFI_BOOT_SERVICES *bs = uefi.bs;
    EFI_LOADED_IMAGE_PROTOCOL *loaded_image = NULL;

    EFI_GUID *guid = (EFI_GUID *)(uintptr_t)&loaded_image_guid;
    void **out = (void **)&loaded_image;

    EFI_STATUS status = bs->HandleProtocol(image, guid, out);

    if (efi_error(status) || !loaded_image) {
        return status;
    }

    u64 image_base = (u64)(uintptr_t)loaded_image->ImageBase;
    u64 image_size = loaded_image->ImageSize;

    if (image_base && image_size) {
        status = map_boot(image_base, image_size, PT_WRITE);

        if (efi_error(status)) {
            return status;
        }
    }

    u64 rsp = 0;
    asm volatile("mov %%rsp, %0" : "=r"(rsp));

    const u64 stack_window = 128 * 1024;
    u64 stack_base = 0;

    if (rsp > stack_window / 2) {
        stack_base = rsp - stack_window / 2;
    }

    status = map_boot(stack_base, stack_window, PT_WRITE);

    if (efi_error(status)) {
        return status;
    }

    return EFI_SUCCESS;
}

static EFI_STATUS map_fb(const boot_info_t *info) {
    if (!info || info->video.mode != VIDEO_GRAPHICS || !info->video.framebuffer || !info->video.width ||
        !info->video.height || !info->video.bytes_per_pixel) {
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

    u64 base_2m = ALIGN_DOWN(fb_start, PAGE_2MIB);
    u64 end_2m = ALIGN(fb_end, PAGE_2MIB);

    for (u64 addr = base_2m; addr < end_2m; addr += PAGE_2MIB) {
        if (addr < PROTECTED_MODE_TOP) {
            EFI_STATUS s = map_page_2m(addr, addr, PT_WRITE | PT_PAT_HUGE);
            if (efi_error(s)) {
                return s;
            }

            s = map_page_2m(addr + LINEAR_MAP_OFFSET_64, addr, PT_WRITE | PT_PAT_HUGE);

            if (efi_error(s)) {
                return s;
            }
        } else {
            u64 chunk_base = ALIGN_DOWN(addr, PAGE_4KIB);
            u64 chunk_end = addr + PAGE_2MIB;

            if (chunk_end > end_2m) {
                chunk_end = end_2m;
            }

            EFI_STATUS s = map_region_4k(
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

static EFI_STATUS load_elf(void *file_data, UINTN file_size, u64 *entry) {
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

    u64 phdr_end = header->phoff + (u64)header->ph_num * (u64)header->phent_size;

    if (phdr_end > file_size) {
        return EFI_LOAD_ERROR;
    }

    for (size_t i = 0; i < header->ph_num; i++) {
        size_t offset = header->phoff + i * header->phent_size;
        elf_prog_header_t *ph = (elf_prog_header_t *)((u8 *)file_data + offset);

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

        EFI_STATUS status = alloc_low(pages, &paddr);

        if (efi_error(status)) {
            return status;
        }

        uefi_mem_zero((void *)(uintptr_t)paddr, pages * EFI_PAGE_SIZE);
        uefi_mem_copy((void *)(uintptr_t)paddr, (u8 *)file_data + ph->offset, ph->file_size);

        u64 flags = 0;
        if (ph->flags & PF_W) {
            flags |= PT_WRITE;
        }

        status = map_region_4k(ph->vaddr, paddr, ph->mem_size, flags);
        if (efi_error(status)) {
            return status;
        }
    }

    *entry = header->entry;
    return EFI_SUCCESS;
}

static void default_args(boot_info_t *info) {
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

    uefi_str_copy(info->args.console, sizeof(info->args.console), "/dev/ttyS0,/dev/console");

    uefi_str_copy(info->args.font, sizeof(info->args.font), BOOT_DEFAULT_FONT);
}

static void init_root_hint(boot_root_hint_t *hint) {
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

static void set_root_hint(boot_info_t *info, const void *rootfs_file, UINTN rootfs_size) {
    if (!info || !rootfs_file || rootfs_size < 1024 + sizeof(ext2_superblock_t)) {
        return;
    }

    init_root_hint(&info->boot_root_hint);

    const u8 *bytes = (const u8 *)rootfs_file;
    const ext2_superblock_t *sb = (const ext2_superblock_t *)(const void *)(bytes + 1024);

    if (sb->signature != EXT2_SIGNATURE) {
        return;
    }

    info->boot_root_hint.rootfs_uuid_valid = 1;
    uefi_mem_copy(info->boot_root_hint.rootfs_uuid, sb->fs_id, sizeof(info->boot_root_hint.rootfs_uuid));
}

static void stage_rootfs(EFI_HANDLE image, boot_info_t *info) {
    if (!image || !info || !uefi.bs) {
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
        status = uefi_load_file(
            uefi.bs,
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

    status = alloc_low(pages, &rootfs_phys);

    if (!efi_error(status)) {
        uefi_mem_zero((void *)(uintptr_t)rootfs_phys, pages * EFI_PAGE_SIZE);
        uefi_mem_copy((void *)(uintptr_t)rootfs_phys, rootfs_file, rootfs_size);
        info->boot_rootfs_paddr = (u64)rootfs_phys;
        info->boot_rootfs_size = (u64)rootfs_size;
        set_root_hint(info, rootfs_file, rootfs_size);
    }

    uefi.bs->FreePool(rootfs_file);
}

static NORETURN void jump_to_kernel(u64 entry, u64 stack_top, boot_info_t *info) {
    asm volatile("cli\n\t"
                 "cld\n\t"
                 "mov %0, %%rsp\n\t"
                 "xor %%rbp, %%rbp\n\t"
                 "jmp *%1"
                 :
                 : "r"(stack_top), "r"(entry), "D"(info)
                 : "memory");

    __builtin_unreachable();
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *system_table) {
    uefi.st = system_table;
    uefi.bs = NULL;

    if (system_table) {
        uefi.bs = system_table->BootServices;
    }

    if (!uefi.bs || !uefi.st) {
        return EFI_LOAD_ERROR;
    }

    log_init(log_sink);
    log_set_lvl(LOG_DEBUG);
    log_set_options(log_opts());

    log_info("UEFI boot started");

    if (uefi.bs->SetWatchdogTimer) {
        uefi.bs->SetWatchdogTimer(0, 0, 0, NULL);
    }

    void *kernel_file = NULL;
    UINTN kernel_file_size = 0;
    EFI_STATUS status = uefi_load_file(
        uefi.bs,
        image,
        &loaded_image_guid,
        &simple_fs_guid,
        &file_info_guid,
        (const CHAR16 *)L"\\boot\\kernel64.elf",
        &kernel_file,
        &kernel_file_size
    );

    if (efi_error(status)) {
        log_error("failed to open kernel64.elf");
        return status;
    }

    EFI_PHYSICAL_ADDRESS info_phys = 0;
    status = alloc_low(1, &info_phys);

    if (efi_error(status)) {
        return fail((const CHAR16 *)L"boot info allocation failed ", status);
    }

    boot_info_t *info = (boot_info_t *)(uintptr_t)info_phys;
    uefi_mem_zero(info, sizeof(*info));
    info->boot_log_paddr = (u64)(uintptr_t)uefi.log;
    info->boot_log_cap = BOOT_LOG_CAP;

    EFI_PHYSICAL_ADDRESS smp_trampoline = 0;
    status = alloc_below(1, 0x000FFFFFULL, &smp_trampoline);
    if (!efi_error(status)) {
        info->smp_trampoline_paddr = (u64)smp_trampoline;
    }

    default_args(info);
    load_conf(image, info);

    uefi_detect_acpi(info, uefi.st, &acpi2_guid, &acpi_guid);
    uefi_detect_video(info, uefi.bs, &gop_guid);

    status = new_table(&uefi.lvl4);
    if (efi_error(status)) {
        return fail((const CHAR16 *)L"page table allocation failed ", status);
    }

    status = map_lowmem();
    if (efi_error(status)) {
        return fail((const CHAR16 *)L"low memory map failed ", status);
    }

    status = map_loader(image);
    if (efi_error(status)) {
        return fail((const CHAR16 *)L"loader runtime map failed ", status);
    }

    status = map_fb(info);
    if (efi_error(status)) {
        return fail((const CHAR16 *)L"framebuffer map failed ", status);
    }

    u64 kernel_entry = 0;
    status = load_elf(kernel_file, kernel_file_size, &kernel_entry);

    if (efi_error(status)) {
        log_error("failed to load kernel ELF");
        return status;
    }

    UINTN stack_pages = DIV_ROUND_UP(KERNEL_STACK_SIZE, PAGE_4KIB);
    EFI_PHYSICAL_ADDRESS stack_phys = 0;
    status = alloc_low(stack_pages, &stack_phys);
    if (efi_error(status)) {
        return fail((const CHAR16 *)L"kernel stack allocation failed ", status);
    }

    if (info->args.stage_rootfs) {
        stage_rootfs(image, info);
    }

    u64 stack_top = stack_phys + KERNEL_STACK_SIZE + LINEAR_MAP_OFFSET_64;
    boot_info_t *info_virt = (boot_info_t *)(uintptr_t)(info_phys + LINEAR_MAP_OFFSET_64);

    void *map_buf = NULL;
    UINTN map_buf_size = 0;
    UINTN map_key = 0;

    for (int attempts = 0; attempts < 8; attempts++) {
        status = uefi_memory_map(uefi.bs, info, &map_buf, &map_buf_size, &map_key);

        if (efi_error(status)) {
            return fail((const CHAR16 *)L"memory map fetch failed ", status);
        }

        status = uefi.bs->ExitBootServices(image, map_key);
        if (!efi_error(status)) {
            break;
        }
    }

    if (efi_error(status)) {
        return fail((const CHAR16 *)L"ExitBootServices failed ", status);
    }

    info->boot_log_len = (u32)uefi.log_len;
    pat_init();

    write_cr3((u64)(uintptr_t)uefi.lvl4);

    u64 cr0 = read_cr0();
    write_cr0(cr0 | CR0_WP);

    jump_to_kernel(kernel_entry, stack_top, info_virt);
}
