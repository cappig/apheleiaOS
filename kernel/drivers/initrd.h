#pragma once

#include <base/types.h>
#include <boot/proto.h>
#include <fs/ustar.h>


void ustar_init(void);

void initrd_mount(boot_handoff* handoff);
