#include <arch/arch.h>
#include <base/attributes.h>
#include <fs/ext2fs.h>
#include <log/log.h>
#include <sched/scheduler.h>
#include <sys/devfs.h>
#include <sys/disk.h>
#include <sys/init.h>
#include <sys/psf.h>
#include <sys/symbols.h>
#include <sys/syscall.h>
#include <sys/vfs.h>

NORETURN void kernel_main(void* boot_info) {
    arch_init(boot_info);
    scheduler_init();
    syscall_init();
    vfs_init();
    ext2fs_init();

    arch_storage_init();

    disk_dev_t* boot_disk = disk_lookup(1);
    if (!boot_disk || !mount_rootfs(boot_disk)) {
        log_warn("kernel: failed to mount rootfs");
    } else {
        load_symbols();
    }

    const char* font_path = arch_font_path();
    if (font_path && font_path[0]) {
        if (!psf_load(font_path))
            log_warn("kernel: failed to load console font '%s'", font_path);
    }

    disk_publish_devices();
    devfs_init();
    dump_vfs();
    init_spawn();
    scheduler_start();

    for (;;)
        ;
}
