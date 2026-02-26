#pragma once

// Each arch provides <arch_mm.h> defining:
//   arch_alloc_frames_user, arch_free_frames, arch_map_region,
//   arch_get_page, arch_page_get_paddr, arch_page_set_paddr
#include_next <arch_mm.h>
