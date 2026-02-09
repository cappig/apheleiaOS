#include <arch/arch.h>
#include <base/attributes.h>
#include <fs/ext2fs.h>
#include <log/log.h>
#include <sys/devfs.h>
#include <sys/disk.h>
#include <sys/vfs.h>

NORETURN void kernel_main(void* boot_info) {
    arch_init(boot_info);
    vfs_init();
    ext2fs_init();

    arch_storage_init();

    disk_dev_t* boot_disk = disk_lookup(1);
    if (!boot_disk || !mount_rootfs(boot_disk))
        log_warn("kernel: failed to mount rootfs");

    disk_publish_devices();
    devfs_init();
    dump_vfs();

    for (;;)
        ;
}
