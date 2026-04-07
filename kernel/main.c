#include <arch/arch.h>
#include <base/attributes.h>
#include <drivers/manager.h>
#include <fs/ext2fs.h>
#include <log/log.h>
#include <sched/scheduler.h>
#include <sys/devfs.h>
#include <sys/disk.h>
#include <sys/framebuffer.h>
#include <sys/init.h>
#include <sys/cpu.h>
#include <sys/logsink.h>
#include <sys/psf.h>
#include <sys/pty.h>
#include <sys/procfs.h>
#include <sys/panic.h>
#include <sys/symbols.h>
#include <sys/syscall.h>
#include <sys/tty.h>
#include <sys/vfs.h>

#include "sys/ws.h"

NORETURN void kernel_main(void *boot_info) {
    const kernel_args_t *args = arch_init(boot_info);

    scheduler_init();
    syscall_init();
    vfs_init();
    ext2fs_init();

    arch_storage_init();

    bool mounted = mount_rootfs();

    if (!mounted) {
        panic("failed to mount rootfs");
    }

    load_symbols();

    if (!procfs_init()) {
        log_warn("procfs init failed");
    }

    psf_load_boot_font(args ? args->font : NULL);

    tty_init();
    pty_init();

    if (framebuffer_get_info()) {
        if (!ws_init()) {
            log_warn("ws init failed");
        }
    }

    devfs_init();
    disk_publish_devices();
    driver_load_stage(DRIVER_STAGE_DEVFS);

    logsink_bind_devices();

    init_spawn();
    arch_late_init();
    arch_smp_init();
    scheduler_start();

    cpu_halt();
}
