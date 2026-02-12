#include "util.h"

#include <x86/boot.h>
#include <x86/e820.h>
#include <x86/vga.h>

bool uefi_guid_eq(const EFI_GUID* a, const EFI_GUID* b) {
    if (!a || !b)
        return false;

    if (a->Data1 != b->Data1 || a->Data2 != b->Data2 || a->Data3 != b->Data3)
        return false;

    for (size_t i = 0; i < 8; i++) {
        if (a->Data4[i] != b->Data4[i])
            return false;
    }

    return true;
}

void uefi_mem_zero(void* dst, size_t len) {
    u8* out = (u8*)dst;

    for (size_t i = 0; i < len; i++)
        out[i] = 0;
}

void uefi_mem_copy(void* dst, const void* src, size_t len) {
    u8* out = (u8*)dst;
    const u8* in = (const u8*)src;

    for (size_t i = 0; i < len; i++)
        out[i] = in[i];
}

void uefi_str_copy(char* dst, size_t cap, const char* src) {
    if (!dst || !cap)
        return;

    size_t i = 0;
    if (src) {
        while (src[i] && i + 1 < cap) {
            dst[i] = src[i];
            i++;
        }
    }

    dst[i] = '\0';
}

static u32 _efi_mem_type_to_e820(u32 type) {
    switch (type) {
    case EfiConventionalMemory:
        return E820_AVAILABLE;
    case EfiACPIReclaimMemory:
        return E820_ACPI;
    case EfiACPIMemoryNVS:
        return E820_NVS;
    default:
        return E820_RESERVED;
    }
}

static void
_build_e820_map(boot_info_t* info, EFI_MEMORY_DESCRIPTOR* descs, UINTN map_size, UINTN desc_size) {
    if (!info || !descs || !desc_size)
        return;

    info->memory_map.count = 0;

    UINTN count = map_size / desc_size;
    for (UINTN i = 0; i < count; i++) {
        EFI_MEMORY_DESCRIPTOR* desc = (EFI_MEMORY_DESCRIPTOR*)((u8*)descs + i * desc_size);

        u64 base = desc->PhysicalStart;
        u64 size = desc->NumberOfPages * EFI_PAGE_SIZE;

        if (!size || base >= PROTECTED_MODE_TOP)
            continue;

        if (base + size > PROTECTED_MODE_TOP)
            size = PROTECTED_MODE_TOP - base;

        u32 type = _efi_mem_type_to_e820(desc->Type);

        if (info->memory_map.count > 0) {
            e820_entry_t* prev = &info->memory_map.entries[info->memory_map.count - 1];
            u64 prev_top = prev->address + prev->size;

            if (prev->type == type && prev_top == base) {
                prev->size += size;
                continue;
            }
        }

        if (info->memory_map.count >= E820_MAX)
            break;

        e820_entry_t* entry = &info->memory_map.entries[info->memory_map.count++];
        entry->address = base;
        entry->size = size;
        entry->type = type;
        entry->acpi = 0;
    }
}

EFI_STATUS uefi_get_memory_map_and_key(
    EFI_BOOT_SERVICES* bs,
    boot_info_t* info,
    void** map_buf,
    UINTN* map_buf_size,
    UINTN* map_key
) {
    if (!bs || !info || !map_buf || !map_buf_size || !map_key)
        return EFI_INVALID_PARAM;

    UINTN query_size = 0;
    UINTN query_key = 0;
    UINTN desc_size = 0;
    UINT32 desc_version = 0;

    EFI_STATUS status = bs->GetMemoryMap(&query_size, NULL, &query_key, &desc_size, &desc_version);
    if (status != EFI_BUFFER_SMALL)
        return status;

    query_size += desc_size * 8;

    if (!*map_buf || *map_buf_size < query_size) {
        if (*map_buf)
            bs->FreePool(*map_buf);

        status = bs->AllocatePool(EfiLoaderData, query_size, map_buf);
        if (efi_error(status))
            return status;

        *map_buf_size = query_size;
    }

    UINTN map_size = *map_buf_size;
    status = bs->GetMemoryMap(
        &map_size, (EFI_MEMORY_DESCRIPTOR*)(*map_buf), map_key, &desc_size, &desc_version
    );
    if (efi_error(status))
        return status;

    _build_e820_map(info, (EFI_MEMORY_DESCRIPTOR*)(*map_buf), map_size, desc_size);
    return EFI_SUCCESS;
}

EFI_STATUS uefi_load_file_from_boot_volume(
    EFI_BOOT_SERVICES* bs,
    EFI_HANDLE image,
    const EFI_GUID* loaded_image_guid,
    const EFI_GUID* simple_fs_guid,
    const EFI_GUID* file_info_guid,
    const CHAR16* path,
    void** file_data,
    UINTN* file_size
) {
    if (!bs || !image || !loaded_image_guid || !simple_fs_guid || !file_info_guid || !path ||
        !file_data || !file_size)
        return EFI_INVALID_PARAM;

    EFI_LOADED_IMAGE_PROTOCOL* loaded_image = NULL;
    EFI_STATUS status =
        bs->HandleProtocol(image, (EFI_GUID*)(uintptr_t)loaded_image_guid, (void**)&loaded_image);
    if (efi_error(status))
        return status;

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs = NULL;
    status = bs->HandleProtocol(
        loaded_image->DeviceHandle, (EFI_GUID*)(uintptr_t)simple_fs_guid, (void**)&fs
    );
    if (efi_error(status))
        return status;

    EFI_FILE_PROTOCOL* root = NULL;
    status = fs->OpenVolume(fs, &root);
    if (efi_error(status))
        return status;

    EFI_FILE_PROTOCOL* file = NULL;
    status = root->Open(root, &file, path, EFI_FILE_MODE_READ, 0);
    if (efi_error(status)) {
        root->Close(root);
        return status;
    }

    UINTN info_size = 0;
    status = file->GetInfo(file, (EFI_GUID*)(uintptr_t)file_info_guid, &info_size, NULL);
    if (status != EFI_BUFFER_SMALL) {
        file->Close(file);
        root->Close(root);
        return status;
    }

    EFI_FILE_INFO* info = NULL;
    status = bs->AllocatePool(EfiLoaderData, info_size, (void**)&info);
    if (efi_error(status)) {
        file->Close(file);
        root->Close(root);
        return status;
    }

    status = file->GetInfo(file, (EFI_GUID*)(uintptr_t)file_info_guid, &info_size, info);
    if (efi_error(status)) {
        bs->FreePool(info);
        file->Close(file);
        root->Close(root);
        return status;
    }

    UINTN size = (UINTN)info->FileSize;
    bs->FreePool(info);

    void* buffer = NULL;
    status = bs->AllocatePool(EfiLoaderData, size, &buffer);
    if (efi_error(status)) {
        file->Close(file);
        root->Close(root);
        return status;
    }

    status = file->Read(file, &size, buffer);
    file->Close(file);
    root->Close(root);

    if (efi_error(status)) {
        bs->FreePool(buffer);
        return status;
    }

    *file_data = buffer;
    *file_size = size;
    return EFI_SUCCESS;
}

void uefi_detect_acpi(
    boot_info_t* info,
    EFI_SYSTEM_TABLE* st,
    const EFI_GUID* acpi2_guid,
    const EFI_GUID* acpi_guid
) {
    if (!info || !st || !st->ConfigurationTable || !acpi2_guid || !acpi_guid)
        return;

    info->acpi_root_ptr = 0;

    for (UINTN i = 0; i < st->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE* table = &st->ConfigurationTable[i];
        if (uefi_guid_eq(&table->VendorGuid, acpi2_guid)) {
            info->acpi_root_ptr = (u64)(uintptr_t)table->VendorTable;
            return;
        }
    }

    for (UINTN i = 0; i < st->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE* table = &st->ConfigurationTable[i];
        if (uefi_guid_eq(&table->VendorGuid, acpi_guid)) {
            info->acpi_root_ptr = (u64)(uintptr_t)table->VendorTable;
            return;
        }
    }
}

void uefi_detect_video(boot_info_t* info, EFI_BOOT_SERVICES* bs, const EFI_GUID* gop_guid) {
    if (!info)
        return;

    uefi_mem_zero(&info->video, sizeof(info->video));
    info->video.mode = VIDEO_TEXT;
    info->video.framebuffer = VGA_ADDR;
    info->video.width = VGA_WIDTH;
    info->video.height = VGA_HEIGHT;
    info->video.bytes_per_pixel = 2;
    info->video.bytes_per_line = VGA_WIDTH * 2;

    if (!bs || !gop_guid)
        return;

    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop = NULL;
    EFI_STATUS status = bs->LocateProtocol((EFI_GUID*)(uintptr_t)gop_guid, NULL, (void**)&gop);
    if (efi_error(status) || !gop || !gop->Mode || !gop->Mode->Info)
        return;

    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* mode = gop->Mode->Info;
    if (!mode || !gop->Mode->FrameBufferBase)
        return;

    if (gop->Mode->FrameBufferBase > 0xffffffffULL)
        return;

    info->video.mode = VIDEO_GRAPHICS;
    info->video.framebuffer = (u32)gop->Mode->FrameBufferBase;
    info->video.width = (u16)mode->HorizontalResolution;
    info->video.height = (u16)mode->VerticalResolution;
    info->video.bytes_per_pixel = 4;
    info->video.bytes_per_line = (u16)(mode->PixelsPerScanLine * 4);
    info->video.red_mask = 8;
    info->video.green_mask = 8;
    info->video.blue_mask = 8;
}
