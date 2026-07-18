#pragma once

#include <arch/paging.h>

void vm_init_kernel(page_t *root, bool use_asids);
