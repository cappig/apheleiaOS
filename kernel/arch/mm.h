#pragma once

#include <arch/paging.h>
#include <base/types.h>

// Each arch provides <arch_mm.h> defining inline wrappers:
//   arch_alloc_frames_user, arch_free_frames, arch_map_region,
//   arch_get_page, arch_page_get_paddr, arch_page_set_paddr
#include <arch_mm.h>
