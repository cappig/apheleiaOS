#pragma once

#include <base/types.h>
#include <boot/proto.h>


void* initrd_find(const char* name);

void initrd_init(boot_handoff* handoff);
void initrd_close(boot_handoff* handoff);
