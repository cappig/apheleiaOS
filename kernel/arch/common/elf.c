#include "elf.h"

#include <parse/elf.h>

static bool elf_range_valid(size_t size, u64 offset, u64 length) {
    if (!size) {
        return true;
    }

    u64 limit = (u64)size;
    return offset <= limit && length <= limit - offset;
}

static bool
elf_table_range_valid(size_t size, u64 offset, u64 count, u64 entry_size) {
    if (!size) {
        return true;
    }

    if (count && entry_size > (u64)-1 / count) {
        return false;
    }

    return elf_range_valid(size, offset, count * entry_size);
}

static bool elf_walk_32(
    const u8 *blob,
    size_t size,
    elf_segment_cb cb,
    void *ctx,
    elf_info_t *out_info
) {
    const elf32_header_t *eh = (const elf32_header_t *)blob;

    if (size && size < sizeof(*eh)) {
        return false;
    }

    if (eh->phent_size < sizeof(elf32_prog_header_t)) {
        return false;
    }

    if (!elf_table_range_valid(size, eh->phoff, eh->ph_num, eh->phent_size)) {
        return false;
    }

    if (out_info) {
        out_info->entry = eh->entry;
        out_info->is_64 = false;
    }

    for (u16 i = 0; i < eh->ph_num; i++) {
        u32 ph_off = eh->phoff + (u32)i * eh->phent_size;
        const elf32_prog_header_t *ph =
            (const elf32_prog_header_t *)(blob + ph_off);

        if (ph->type != PT_LOAD) {
            continue;
        }

        if (!ph->file_size && !ph->mem_size) {
            continue;
        }

        if (!elf_range_valid(size, ph->offset, ph->file_size)) {
            return false;
        }

        elf_segment_t seg = {
            .vaddr = ph->vaddr,
            .paddr = ph->paddr,
            .offset = ph->offset,
            .file_size = ph->file_size,
            .mem_size = ph->mem_size,
            .align = ph->align,
            .flags = ph->flags,
        };

        if (!cb(&seg, ctx)) {
            return false;
        }
    }

    return true;
}

static bool elf_walk_64(
    const u8 *blob,
    size_t size,
    elf_segment_cb cb,
    void *ctx,
    elf_info_t *out_info
) {
    const elf_header_t *eh = (const elf_header_t *)blob;

    if (size && size < sizeof(*eh)) {
        return false;
    }

    if (eh->phent_size < sizeof(elf_prog_header_t)) {
        return false;
    }

    if (!elf_table_range_valid(size, eh->phoff, eh->ph_num, eh->phent_size)) {
        return false;
    }

    if (out_info) {
        out_info->entry = eh->entry;
        out_info->is_64 = true;
    }

    for (u16 i = 0; i < eh->ph_num; i++) {
        u64 ph_off = eh->phoff + (u64)i * eh->phent_size;
        const elf_prog_header_t *ph =
            (const elf_prog_header_t *)(blob + ph_off);

        if (ph->type != PT_LOAD) {
            continue;
        }

        if (!ph->file_size && !ph->mem_size) {
            continue;
        }

        if (!elf_range_valid(size, ph->offset, ph->file_size)) {
            return false;
        }

        elf_segment_t seg = {
            .vaddr = ph->vaddr,
            .paddr = ph->paddr,
            .offset = ph->offset,
            .file_size = ph->file_size,
            .mem_size = ph->mem_size,
            .align = ph->align,
            .flags = ph->flags,
        };

        if (!cb(&seg, ctx)) {
            return false;
        }
    }

    return true;
}

bool elf_foreach_segment(
    const void *elf,
    size_t size,
    elf_segment_cb cb,
    void *ctx,
    elf_info_t *out_info
) {
    if (!elf || !cb) {
        return false;
    }

    if (size && size < sizeof(elf32_header_t)) {
        return false;
    }

    const elf32_header_t *eh32 = (const elf32_header_t *)elf;
    if (eh32->magic != ELF_MAGIC || eh32->endianness != EEND_LITTLE) {
        return false;
    }

    if (eh32->arch == EARCH_64) {
        return elf_walk_64((const u8 *)elf, size, cb, ctx, out_info);
    }

    if (eh32->arch == EARCH_32) {
        return elf_walk_32((const u8 *)elf, size, cb, ctx, out_info);
    }

    return false;
}
