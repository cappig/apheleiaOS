#include <base/attributes.h>
#include <base/macros.h>
#include <base/types.h>
#include <lib/boot.h>
#include <parse/elf.h>
#include <x86/asm.h>
#include <x86/boot.h>
#include <x86/paging64.h>

#include "efi.h"
#include "util.h"

static EFI_SYSTEM_TABLE* g_st = NULL;
static EFI_BOOT_SERVICES* g_bs = NULL;
static page_t* g_lvl4 = NULL;

static const EFI_GUID loaded_image_guid =
    {0x5b1b31a1, 0x9562, 0x11d2, {0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
static const EFI_GUID simple_fs_guid =
    {0x0964e5b22, 0x6459, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
static const EFI_GUID file_info_guid =
    {0x09576e92, 0x6d3f, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
static const EFI_GUID gop_guid =
    {0x9042a9de, 0x23dc, 0x4a38, {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}};
static const EFI_GUID acpi_guid =
    {0xeb9d2d30, 0x2d88, 0x11d3, {0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}};
static const EFI_GUID acpi2_guid =
    {0x8868e871, 0xe4f1, 0x11d3, {0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81}};

static void _uefi_puts(const CHAR16* s) {
    if (!s || !g_st || !g_st->ConOut || !g_st->ConOut->OutputString)
        return;

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

static EFI_STATUS _uefi_fail(const CHAR16* msg, EFI_STATUS status) {
    if (msg)
        _uefi_puts(msg);

    _uefi_put_hex64(status);
    _uefi_puts((const CHAR16*)L"\r\n");

    if (g_bs && g_bs->Stall) {
        typedef EFI_STATUS(EFIAPI * efi_stall_t)(UINTN);
        ((efi_stall_t)g_bs->Stall)(3000000);
    }

    return status;
}

static EFI_STATUS _alloc_pages_low(UINTN pages, EFI_PHYSICAL_ADDRESS* addr) {
    if (!addr)
        return EFI_INVALID_PARAM;

    *addr = 0xffffffffULL;
    return g_bs->AllocatePages(AllocateMaxAddress, EfiLoaderData, pages, addr);
}

static EFI_STATUS _alloc_table(page_t** table) {
    if (!table)
        return EFI_INVALID_PARAM;

    EFI_PHYSICAL_ADDRESS addr = 0;
    EFI_STATUS status = _alloc_pages_low(1, &addr);
    if (efi_error(status))
        return status;

    uefi_mem_zero((void*)(uintptr_t)addr, EFI_PAGE_SIZE);
    *table = (page_t*)(uintptr_t)addr;
    return EFI_SUCCESS;
}

static EFI_STATUS _walk_table(page_t* table, size_t index, page_t** next) {
    if (!table || !next)
        return EFI_INVALID_PARAM;

    if (table[index] & PT_PRESENT) {
        *next = (page_t*)(uintptr_t)page_get_paddr(&table[index]);
        return EFI_SUCCESS;
    }

    page_t* child = NULL;
    EFI_STATUS status = _alloc_table(&child);
    if (efi_error(status))
        return status;

    table[index] = ((u64)(uintptr_t)child & ADDR_MASK) | PT_PRESENT | PT_WRITE;
    *next = child;
    return EFI_SUCCESS;
}

static EFI_STATUS _map_page_4k(u64 vaddr, u64 paddr, u64 flags) {
    page_t* lvl3 = NULL;
    page_t* lvl2 = NULL;
    page_t* lvl1 = NULL;

    EFI_STATUS status = _walk_table(g_lvl4, GET_LVL4_INDEX(vaddr), &lvl3);
    if (efi_error(status))
        return status;

    status = _walk_table(lvl3, GET_LVL3_INDEX(vaddr), &lvl2);
    if (efi_error(status))
        return status;

    status = _walk_table(lvl2, GET_LVL2_INDEX(vaddr), &lvl1);
    if (efi_error(status))
        return status;

    size_t index = GET_LVL1_INDEX(vaddr);
    lvl1[index] = (paddr & ADDR_MASK) | (flags & FLAGS_MASK) | PT_PRESENT;
    return EFI_SUCCESS;
}

static EFI_STATUS _map_page_2m(u64 vaddr, u64 paddr, u64 flags) {
    page_t* lvl3 = NULL;
    page_t* lvl2 = NULL;

    EFI_STATUS status = _walk_table(g_lvl4, GET_LVL4_INDEX(vaddr), &lvl3);
    if (efi_error(status))
        return status;

    status = _walk_table(lvl3, GET_LVL3_INDEX(vaddr), &lvl2);
    if (efi_error(status))
        return status;

    size_t index = GET_LVL2_INDEX(vaddr);
    lvl2[index] = (paddr & ADDR_MASK) | (flags & FLAGS_MASK) | PT_PRESENT | PT_HUGE;
    return EFI_SUCCESS;
}

static EFI_STATUS _map_region_4k(u64 vaddr, u64 paddr, u64 size, u64 flags) {
    u64 pages = DIV_ROUND_UP(size, PAGE_4KIB);

    for (u64 i = 0; i < pages; i++) {
        EFI_STATUS status = _map_page_4k(vaddr + i * PAGE_4KIB, paddr + i * PAGE_4KIB, flags);
        if (efi_error(status))
            return status;
    }

    return EFI_SUCCESS;
}

static EFI_STATUS _map_identity_and_linear(void) {
    for (u64 addr = 0; addr < PROTECTED_MODE_TOP; addr += PAGE_2MIB) {
        EFI_STATUS status = _map_page_2m(addr, addr, PT_WRITE);
        if (efi_error(status))
            return status;

        status = _map_page_2m(addr + LINEAR_MAP_OFFSET_64, addr, PT_WRITE);
        if (efi_error(status))
            return status;
    }

    return EFI_SUCCESS;
}

static EFI_STATUS _map_region_identity_and_linear(u64 paddr, u64 size, u64 flags) {
    if (!size)
        return EFI_SUCCESS;

    u64 base = ALIGN_DOWN(paddr, PAGE_4KIB);
    u64 end = ALIGN(paddr + size, PAGE_4KIB);

    if (end <= base)
        return EFI_SUCCESS;

    EFI_STATUS status = _map_region_4k(base, base, end - base, flags);
    if (efi_error(status))
        return status;

    return _map_region_4k(base + LINEAR_MAP_OFFSET_64, base, end - base, flags);
}

static EFI_STATUS _map_loader_runtime(EFI_HANDLE image) {
    if (!image || !g_bs)
        return EFI_INVALID_PARAM;

    EFI_LOADED_IMAGE_PROTOCOL* loaded_image = NULL;
    EFI_STATUS status = g_bs->HandleProtocol(
        image,
        (EFI_GUID*)(uintptr_t)&loaded_image_guid,
        (void**)&loaded_image
    );

    if (efi_error(status) || !loaded_image)
        return status;

    u64 image_base = (u64)(uintptr_t)loaded_image->ImageBase;
    u64 image_size = loaded_image->ImageSize;

    if (image_base && image_size) {
        status = _map_region_identity_and_linear(image_base, image_size, PT_WRITE);

        if (efi_error(status))
            return status;
    }

    u64 rsp = 0;
    asm volatile("mov %%rsp, %0" : "=r"(rsp));

    const u64 stack_window = 128 * 1024;
    u64 stack_base = (rsp > stack_window / 2) ? (rsp - stack_window / 2) : 0;

    status = _map_region_identity_and_linear(stack_base, stack_window, PT_WRITE);

    if (efi_error(status))
        return status;

    return EFI_SUCCESS;
}

static EFI_STATUS _map_framebuffer_linear(const boot_info_t* info) {
    if (!info || info->video.mode != VIDEO_GRAPHICS || !info->video.framebuffer ||
        !info->video.width || !info->video.height || !info->video.bytes_per_pixel)
        return EFI_SUCCESS;

    u64 pitch = info->video.bytes_per_line;
    if (!pitch)
        pitch = (u64)info->video.width * info->video.bytes_per_pixel;

    u64 fb_size = pitch * info->video.height;
    if (!fb_size)
        return EFI_SUCCESS;

    u64 fb_base = ALIGN_DOWN(info->video.framebuffer, PAGE_4KIB);
    u64 fb_end = ALIGN(info->video.framebuffer + fb_size, PAGE_4KIB);

    if (fb_end <= PROTECTED_MODE_TOP)
        return EFI_SUCCESS;

    if (fb_base < PROTECTED_MODE_TOP)
        fb_base = PROTECTED_MODE_TOP;

    return _map_region_4k(
        fb_base + LINEAR_MAP_OFFSET_64,
        fb_base,
        fb_end - fb_base,
        PT_WRITE | PT_NO_CACHE
    );
}


static EFI_STATUS _load_kernel_elf(void* file_data, UINTN file_size, u64* entry) {
    if (!file_data || !entry || file_size < sizeof(elf_header_t))
        return EFI_LOAD_ERROR;

    elf_header_t* header = (elf_header_t*)file_data;

    if (header->magic != ELF_MAGIC || header->version != ELF_VERSION)
        return EFI_LOAD_ERROR;

    if (header->endianness != EEND_LITTLE)
        return EFI_LOAD_ERROR;

    if (header->arch != EARCH_64 || header->machine != EM_X86_64)
        return EFI_LOAD_ERROR;

    if (!header->phoff || !header->ph_num || !header->phent_size)
        return EFI_LOAD_ERROR;

    u64 phdr_end = header->phoff + (u64)header->ph_num * (u64)header->phent_size;
    if (phdr_end > file_size)
        return EFI_LOAD_ERROR;

    for (size_t i = 0; i < header->ph_num; i++) {
        elf_prog_header_t* ph =
            (elf_prog_header_t*)((u8*)file_data + header->phoff + i * header->phent_size);

        if (ph->type != PT_LOAD)
            continue;

        if (!ph->mem_size)
            continue;

        u64 seg_end = ph->offset + ph->file_size;
        if (seg_end > file_size)
            return EFI_LOAD_ERROR;

        UINTN pages = (UINTN)DIV_ROUND_UP(ph->mem_size, PAGE_4KIB);
        EFI_PHYSICAL_ADDRESS paddr = 0;
        EFI_STATUS status = _alloc_pages_low(pages, &paddr);
        if (efi_error(status))
            return status;

        uefi_mem_zero((void*)(uintptr_t)paddr, pages * EFI_PAGE_SIZE);
        uefi_mem_copy((void*)(uintptr_t)paddr, (u8*)file_data + ph->offset, ph->file_size);

        u64 flags = 0;
        if (ph->flags & PF_W)
            flags |= PT_WRITE;

        status = _map_region_4k(ph->vaddr, paddr, ph->mem_size, flags);
        if (efi_error(status))
            return status;
    }

    *entry = header->entry;
    return EFI_SUCCESS;
}

static void _setup_default_args(boot_info_t* info) {
    if (!info)
        return;

    uefi_mem_zero(&info->args, sizeof(info->args));
    info->args.debug = DEBUG_ALL;
    info->args.video = BOOT_DEFAULT_VIDEO;
    info->args.vesa_width = (u16)BOOT_DEFAULT_VESA_WIDTH;
    info->args.vesa_height = (u16)BOOT_DEFAULT_VESA_HEIGHT;
    info->args.vesa_bpp = BOOT_DEFAULT_VESA_BPP;
    uefi_str_copy(info->args.console, sizeof(info->args.console), "/dev/ttyS0,/dev/console");
    uefi_str_copy(info->args.font, sizeof(info->args.font), BOOT_DEFAULT_FONT);
}

static void _stage_rootfs_from_esp(EFI_HANDLE image, boot_info_t* info) {
    if (!image || !info || !g_bs)
        return;

    void* rootfs_file = NULL;
    UINTN rootfs_size = 0;

    EFI_STATUS status = uefi_load_file_from_boot_volume(
        g_bs,
        image,
        &loaded_image_guid,
        &simple_fs_guid,
        &file_info_guid,
        (const CHAR16*)L"\\boot\\rootfs.ext2",
        &rootfs_file,
        &rootfs_size
    );

    if (efi_error(status) || !rootfs_file || !rootfs_size) {
        rootfs_file = NULL;
        rootfs_size = 0;

        status = uefi_load_file_from_boot_volume(
            g_bs,
            image,
            &loaded_image_guid,
            &simple_fs_guid,
            &file_info_guid,
            (const CHAR16*)L"\\rootfs.ext2",
            &rootfs_file,
            &rootfs_size
        );
    }

    if (efi_error(status) || !rootfs_file || !rootfs_size)
        return;

    UINTN pages = (UINTN)DIV_ROUND_UP(rootfs_size, PAGE_4KIB);
    EFI_PHYSICAL_ADDRESS rootfs_phys = 0;

    status = _alloc_pages_low(pages, &rootfs_phys);

    if (!efi_error(status)) {
        uefi_mem_zero((void*)(uintptr_t)rootfs_phys, pages * EFI_PAGE_SIZE);
        uefi_mem_copy((void*)(uintptr_t)rootfs_phys, rootfs_file, rootfs_size);
        info->boot_rootfs_paddr = (u64)rootfs_phys;
        info->boot_rootfs_size = (u64)rootfs_size;
    }

    g_bs->FreePool(rootfs_file);
}

static NORETURN void _jump_to_kernel(u64 entry, u64 stack_top, boot_info_t* info) {
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

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE* system_table) {
    g_st = system_table;
    g_bs = system_table ? system_table->BootServices : NULL;

    if (!g_bs || !g_st)
        return EFI_LOAD_ERROR;

    _uefi_puts((const CHAR16*)L"apheleiaOS UEFI: start\r\n");

    if (g_bs->SetWatchdogTimer)
        g_bs->SetWatchdogTimer(0, 0, 0, NULL);

    void* kernel_file = NULL;
    UINTN kernel_file_size = 0;
    EFI_STATUS status = uefi_load_file_from_boot_volume(
        g_bs,
        image,
        &loaded_image_guid,
        &simple_fs_guid,
        &file_info_guid,
        (const CHAR16*)L"\\boot\\kernel64.elf",
        &kernel_file,
        &kernel_file_size
    );

    if (efi_error(status)) {
        _uefi_puts((const CHAR16*)L"apheleiaOS: failed to open kernel64.elf\r\n");
        return status;
    }

    EFI_PHYSICAL_ADDRESS info_phys = 0;
    status = _alloc_pages_low(1, &info_phys);

    if (efi_error(status))
        return _uefi_fail((const CHAR16*)L"apheleiaOS: alloc boot info failed: ", status);

    boot_info_t* info = (boot_info_t*)(uintptr_t)info_phys;
    uefi_mem_zero(info, sizeof(*info));

    _setup_default_args(info);

    uefi_detect_acpi(info, g_st, &acpi2_guid, &acpi_guid);
    uefi_detect_video(info, g_bs, &gop_guid);

    status = _alloc_table(&g_lvl4);
    if (efi_error(status))
        return _uefi_fail((const CHAR16*)L"apheleiaOS: alloc page tables failed: ", status);

    status = _map_identity_and_linear();
    if (efi_error(status))
        return _uefi_fail((const CHAR16*)L"apheleiaOS: map low memory failed: ", status);

    status = _map_loader_runtime(image);
    if (efi_error(status))
        return _uefi_fail((const CHAR16*)L"apheleiaOS: map loader runtime failed: ", status);

    status = _map_framebuffer_linear(info);
    if (efi_error(status))
        return _uefi_fail((const CHAR16*)L"apheleiaOS: map framebuffer failed: ", status);

    u64 kernel_entry = 0;
    status = _load_kernel_elf(kernel_file, kernel_file_size, &kernel_entry);

    if (efi_error(status)) {
        _uefi_puts((const CHAR16*)L"apheleiaOS: failed to load kernel ELF\r\n");
        return status;
    }

    UINTN stack_pages = DIV_ROUND_UP(KERNEL_STACK_SIZE, PAGE_4KIB);
    EFI_PHYSICAL_ADDRESS stack_phys = 0;
    status = _alloc_pages_low(stack_pages, &stack_phys);
    if (efi_error(status))
        return _uefi_fail((const CHAR16*)L"apheleiaOS: alloc kernel stack failed: ", status);

    _stage_rootfs_from_esp(image, info);

    u64 stack_top = stack_phys + KERNEL_STACK_SIZE + LINEAR_MAP_OFFSET_64;
    boot_info_t* info_virt = (boot_info_t*)(uintptr_t)(info_phys + LINEAR_MAP_OFFSET_64);

    void* map_buf = NULL;
    UINTN map_buf_size = 0;
    UINTN map_key = 0;

    for (int attempts = 0; attempts < 8; attempts++) {
        status = uefi_get_memory_map_and_key(g_bs, info, &map_buf, &map_buf_size, &map_key);
        if (efi_error(status))
            return _uefi_fail((const CHAR16*)L"apheleiaOS: get memory map failed: ", status);

        status = g_bs->ExitBootServices(image, map_key);
        if (!efi_error(status))
            break;
    }

    if (efi_error(status))
        return _uefi_fail((const CHAR16*)L"apheleiaOS: ExitBootServices failed: ", status);

    write_cr3((u64)(uintptr_t)g_lvl4);

    u64 cr0 = read_cr0();
    write_cr0(cr0 | CR0_WP);

    _jump_to_kernel(kernel_entry, stack_top, info_virt);
}
