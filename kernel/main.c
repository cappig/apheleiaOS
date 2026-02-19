#include <arch/arch.h>
#include <base/attributes.h>
#include <fs/ext2fs.h>
#include <log/log.h>
#include <sched/scheduler.h>
#include <sys/devfs.h>
#include <sys/disk.h>
#include <sys/framebuffer.h>
#include <sys/init.h>
#include <sys/keyboard.h>
#include <sys/logsink.h>
#include <sys/mouse.h>
#include <sys/psf.h>
#include <sys/pty.h>
#include <sys/symbols.h>
#include <sys/syscall.h>
#include <sys/tty.h>
#include <sys/vfs.h>

#include "sys/input.h"
#include "sys/ws.h"

NORETURN void kernel_main(void *boot_info) {
    const kernel_args_t *args = arch_init(boot_info);
    scheduler_init();
    syscall_init();
    vfs_init();
    ext2fs_init();

    arch_storage_init();

    bool mounted = false;

    for (size_t id = 1; !mounted; id++) {
        disk_dev_t *dev = disk_lookup(id);

        if (!dev) {
            break;
        }

        mounted = mount_rootfs(dev);
    }

    if (!mounted) {
        log_warn("failed to mount rootfs");
    } else {
        load_symbols();
    }

    const char *font_path = args ? args->font : NULL;

    if (font_path && font_path[0]) {
        if (!psf_load(font_path)) {
            log_warn("failed to load console font '%s'", font_path);
        }
    }

    tty_init();
    pty_init();

    if (!input_init()) {
        log_warn("input init failed");
    }

    if (!keyboard_init()) {
        log_warn("keyboard init failed");
    }

    if (!mouse_init()) {
        log_warn("mouse init failed");
    }

    if (!ws_init()) {
        log_warn("ws init failed");
    }

    framebuffer_devfs_init();
    devfs_init();
    disk_publish_devices();
    arch_register_devices();

    logsink_bind_devices();

    init_spawn();
    scheduler_start();

    for (;;) {
        arch_cpu_wait();
    }
}
