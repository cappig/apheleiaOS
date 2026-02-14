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

static void on_signal(int signum) {
    (void)signum;
    exit_requested = 1;
}

int main(void) {
    wm_init();

    signal(SIGINT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);

    int fb_fd = open("/dev/fb", O_RDWR, 0);
    if (fb_fd < 0) {
        io_write_str("wm: open /dev/fb failed\n");
        return 1;
    }

    fb_info_t fb_info = {0};
    if (ioctl(fb_fd, FBIOGETINFO, &fb_info) || !fb_info.available || fb_info.bpp != 32) {
        io_write_str("wm: unsupported framebuffer\n");
        close(fb_fd);
        return 1;
    }

    if (ioctl(fb_fd, FBIOACQUIRE, NULL)) {
        io_write_str("wm: failed to acquire framebuffer\n");
        close(fb_fd);
        return 1;
    }

    ui_t ui = {0};
    if (ui_open(&ui, UI_OPEN_INPUT)) {
        io_write_str("wm: failed to open input/wsctl\n");
        ioctl(fb_fd, FBIORELEASE, NULL);
        close(fb_fd);
        return 1;
    }

    ui_mgr_claim(&ui);

    size_t frame_pixels = (size_t)fb_info.width * (size_t)fb_info.height;
    size_t frame_bytes = frame_pixels * 4;
    if (frame_pixels > WM_MAX_FB_PIX) {
        io_write_str("wm: framebuffer too large\n");
        ui_mgr_release(&ui);
        ui_close(&ui);
        ioctl(fb_fd, FBIORELEASE, NULL);
        close(fb_fd);
        return 1;
    }

    wm_loop(&ui, fb_fd, &fb_info, frame_store, frame_bytes, &exit_requested);

    ui_mgr_release(&ui);
    wm_cleanup_all_windows();
    ui_close(&ui);
    ioctl(fb_fd, FBIORELEASE, NULL);
    close(fb_fd);
    return 0;
}
