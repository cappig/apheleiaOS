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
    bool has_framebuffer = framebuffer_get_info() != NULL;

    scheduler_init();
    syscall_init();
    vfs_init();
    ext2fs_init();

    arch_storage_init();

    bool mounted = mount_rootfs();

    if (!mounted) {
        panic("failed to mount rootfs");
    }

    log_info("initializing tty");
    tty_init();
    pty_init();

    log_info("initializing devfs");
    devfs_init();
    disk_publish_devices();
    driver_load_stage(DRIVER_STAGE_DEVFS);

    log_info("loading kernel symbols");
    load_symbols();

    log_info("initializing procfs");
    if (!procfs_init()) {
        log_warn("procfs init failed");
    }

    if (has_framebuffer) {
        if (args && args->font[0]) {
            log_info("loading console font '%s'", args->font);
        }

        psf_load_boot_font(args ? args->font : NULL);

        log_info("initializing window system");
        if (!ws_init()) {
            log_warn("ws init failed");
        }
    }

    log_info("binding log devices");
    logsink_bind_devices();

    init_spawn();
    arch_late_init();
    arch_smp_init();
    scheduler_start();

    cpu_halt();
}
