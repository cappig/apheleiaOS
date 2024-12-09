#include "tty.h"

#include <base/addr.h>
#include <base/types.h>
#include <boot/proto.h>
#include <data/tree.h>
#include <fs/ustar.h>
#include <gfx/font.h>
#include <gfx/vga.h>
#include <string.h>
#include <term/render.h>
#include <term/term.h>

#include "drivers/initrd.h"
#include "drivers/vesa.h"
#include "log/log.h"
#include "mem/heap.h"
#include "vfs/fs.h"
#include "vfs/pty.h"

gfx_terminal* current_tty = NULL;

// All virtual ttys run in the same mode and have the same size
static bool mode = TERM_RASTER;

static usize width = 0;
static usize height = 0;

static psf_font* font = NULL; // Only for raster terminals


static void _draw_vesa(usize x, usize y, u32 color) {
    rgba_color rgba = {.raw = color};
    draw_pixel_rgba(x, y, rgba);
}

static void _draw_vga(usize x, usize y, u32 color) {
    static volatile u16* vga_base = (u16*)ID_MAPPED_VADDR(VGA_ADDR);

    usize index = x + VGA_WIDTH * y;
    vga_base[index] = (u16)color;
}


static void _pty_write(pseudo_tty* pty, void* buf, usize len) {
    if (!buf || !len)
        return;

    gfx_terminal* gfx_term = pty->private;
    if (!gfx_term)
        return;

    terminal* term = gfx_term->term;
    if (!term)
        return;

    term_parse(term, buf, len);
}


void tty_set_current(gfx_terminal* tty) {
    current_tty = tty;
    tty->draw = true;

    term_redraw(tty->term);
}

gfx_terminal* tty_spawn_size(char* name, usize buffer_size) {
    pseudo_tty* pty = pty_create(buffer_size);

    pty->out_hook = _pty_write;

    term_draw_fn draw_fn = (mode == TERM_RASTER) ? _draw_vesa : _draw_vga;
    gfx_terminal* term = gfx_term_init(width, height, mode, font, draw_fn);

    if (!term)
        return NULL;

    term->draw = false;

    pty->private = term;

    pty->slave->name = strdup(name);

    tree_node* node = tree_create_node(pty->slave);
    vfs_mount("/dev", node);

    return term;
}

gfx_terminal* tty_spawn(char* name) {
    return tty_spawn_size(name, TTY_BUF_SIZE);
}

static bool _load_font(const char* name) {
    if (!name)
        return false;

    void* font_file = initd_find(name);
    if (!font_file)
        return false;

    font = kcalloc(sizeof(psf_font));

    if (!psf_parse(font_file + USTAR_BLOCK_SIZE, font)) {
        kfree(font);
        font = NULL;

        return false;
    }

    return true;
}


void tty_init(graphics_state* gfx_state, boot_handoff* handoff) {
    if (gfx_state->mode == GFX_VESA) {
        if (!_load_font(handoff->args.console_font))
            log_warn("Failed to load tty psf font!");

        width = gfx_state->width;
        height = gfx_state->height;

        mode = TERM_RASTER;
    } else {
        width = VGA_WIDTH;
        height = VGA_HEIGHT;

        mode = TERM_VGA;
    }

    current_tty = NULL;
}

void tty_spawn_devs() {
    gfx_terminal* tty0 = tty_spawn("tty0");
    tty_spawn("tty1");
    tty_spawn("tty2");
    tty_spawn("tty3");

    tty_set_current(tty0);
}
