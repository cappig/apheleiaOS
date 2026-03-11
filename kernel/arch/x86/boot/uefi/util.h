#pragma once

#include <base/types.h>
#include <x86/boot.h>

#include "efi.h"

bool uefi_guid_eq(const EFI_GUID *a, const EFI_GUID *b);

void uefi_mem_zero(void *dst, size_t len);
void uefi_mem_copy(void *dst, const void *src, size_t len);
void uefi_str_copy(char *dst, size_t cap, const char *src);

EFI_STATUS uefi_get_memory_map_and_key(
    EFI_BOOT_SERVICES *bs,
    boot_info_t *info,
    void **map_buf,
    UINTN *map_buf_size,
    UINTN *map_key
);

EFI_STATUS uefi_load_file_from_boot_volume(
    EFI_BOOT_SERVICES *bs,
    EFI_HANDLE image,
    const EFI_GUID *loaded_image_guid,
    const EFI_GUID *simple_fs_guid,
    const EFI_GUID *file_info_guid,
    const CHAR16 *path,
    void **file_data,
    UINTN *file_size
);

void uefi_detect_acpi(
    boot_info_t *info,
    EFI_SYSTEM_TABLE *st,
    const EFI_GUID *acpi2_guid,
    const EFI_GUID *acpi_guid
);

void uefi_detect_video(
    boot_info_t *info,
    EFI_BOOT_SERVICES *bs,
    const EFI_GUID *gop_guid
);
