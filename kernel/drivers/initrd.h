#pragma once

#include <base/types.h>
#include <boot/proto.h>


void* initd_find(const char* name);

void initrd_init(boot_handoff* handoff);
void initrd_close(boot_handoff* handoff);
