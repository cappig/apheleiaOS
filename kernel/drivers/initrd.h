#pragma once

#include <base/types.h>
#include <boot/proto.h>
#include <fs/ustar.h>

#define INITRD_MOUNT "/mnt/initrd"


void initrd_mount(boot_handoff* handoff);
