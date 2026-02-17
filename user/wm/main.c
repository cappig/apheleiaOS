#include <fcntl.h>
#include <gui/fb.h>
#include <signal.h>
#include <stdbool.h>
#include <sys/ioctl.h>
#include <ui.h>
#include <unistd.h>
#include <user/io.h>

#include "wm.h"
#include "wm_loop.h"

static volatile sig_atomic_t exit_requested = 0;
static u32 frame_store[WM_MAX_FB_PIX];

static void _on_signal(int signum) {
    (void)signum;
    exit_requested = 1;
}

int main(void) {
    int ret = 1;
    int fb_fd = -1;

    ui_t ui = {0};

    bool fb_acquired = false;
    bool mgr_claimed = false;

    wm_init();

    signal(SIGINT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTERM, _on_signal);
    signal(SIGHUP, _on_signal);

    fb_fd = open("/dev/fb", O_RDWR, 0);

    if (fb_fd < 0) {
        io_write_str("wm: open /dev/fb failed\n");
        goto out;
    }

    fb_info_t fb_info = {0};
    if (ioctl(fb_fd, FBIOGETINFO, &fb_info) || !fb_info.available || fb_info.bpp != 32) {
        io_write_str("wm: unsupported framebuffer\n");
        goto out;
    }

    size_t packed_row_bytes = (size_t)fb_info.width * 4;
    if (!fb_info.pitch || (size_t)fb_info.pitch < packed_row_bytes) {
        io_write_str("wm: invalid framebuffer pitch\n");
        goto out;
    }

    if (ioctl(fb_fd, FBIOACQUIRE, NULL)) {
        io_write_str("wm: failed to acquire framebuffer\n");
        goto out;
    }

    fb_acquired = true;

    if (ui_open(&ui, UI_OPEN_INPUT)) {
        io_write_str("wm: failed to open input/wsctl\n");
        goto out;
    }

    if (ui_mgr_claim(&ui)) {
        io_write_str("wm: failed to claim ws manager\n");
        goto out;
    }

    mgr_claimed = true;

    size_t frame_pixels = (size_t)fb_info.width * (size_t)fb_info.height;
    size_t frame_bytes = frame_pixels * 4;

    if (frame_pixels > WM_MAX_FB_PIX) {
        io_write_str("wm: framebuffer too large\n");
        goto out;
    }

    wm_loop(&ui, fb_fd, &fb_info, frame_store, frame_bytes, &exit_requested);
    ret = 0;

out:
    if (mgr_claimed)
        ui_mgr_release(&ui);

    wm_cleanup_all_windows();
    ui_close(&ui);

    if (fb_acquired)
        ioctl(fb_fd, FBIORELEASE, NULL);

    if (fb_fd >= 0)
        close(fb_fd);

    return ret;
}
