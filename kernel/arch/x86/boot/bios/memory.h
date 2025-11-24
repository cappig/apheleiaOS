#pragma once

#include <x86/lib/e820.h>


void get_e820(e820_map* mmap);

void get_rsdp(u64* rsdp);
