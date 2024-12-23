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

// The current virtual tty is the one that gets all the user input
// and the one that gets drawn to the screen
pseudo_tty* current_tty = NULL;


// All virtual ttys run in the same mode and have the same size
static bool mode = TERM_RASTER;

static usize width = 0;
static usize height = 0;


static terminal* _get_term(pseudo_tty* pty) {
    if (!pty)
        return NULL;

    gfx_terminal* gterm = pty->private;

    if (!gterm) {
        return NULL;
    }

    return gterm->term;
}


// Only for raster terminals
static psf_font* font = NULL;

static bool _load_font(const char* name) {
    if (!name)
        return false;

    void* font_file = initrd_find(name);

    if (!font_file)
        return false;

    font = kcalloc(sizeof(psf_font));

    if (!psf_parse(font_file, font)) {
        kfree(font);
        font = NULL;

        return false;
    }

    return true;
}


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

    terminal* term = _get_term(pty);

    if (term)
        term_parse(term, buf, len);
}


// A valid pty with a private gfx_term has to be used
bool tty_set_current(pseudo_tty* pty) {
    terminal* term = _get_term(pty);

    if (!term)
        return false;

    current_tty = pty;

    term_reset_style(term); // Do we really need this?
    term_redraw(term);

    log_debug("/dev/%s is now the current virtual tty", pty->slave->name);

    return true;
}

// Send input data to the current tty
void tty_current_input(u8 data) {
    if (!current_tty)
        return;

    vfs_node* node = current_tty->master;
    node->interface->write(node, &data, 0, 1);
}


pseudo_tty* tty_spawn_sized(char* name, usize buffer_size) {
    pseudo_tty* pty = pty_create(buffer_size);

    if (!pty)
        return NULL;

    pty->out_hook = _pty_write;

    pty->flags |= PTY_ECHO;
    pty->flags |= PTY_CANONICAL;

    term_draw_fn draw_fn = (mode == TERM_RASTER) ? _draw_vesa : _draw_vga;
    gfx_terminal* gterm = gfx_term_init(width, height, mode, font, draw_fn);

    if (!gterm)
        return NULL;

    gterm->draw = true;

    pty->private = gterm;

    pty->slave->name = strdup(name);

    tree_node* node = tree_create_node(pty->slave);
    vfs_mount("/dev", node);

    return pty;
}

pseudo_tty* tty_spawn(char* name) {
    return tty_spawn_sized(name, TTY_BUF_SIZE);
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
    tty_set_current(tty_spawn("tty0"));

    tty_spawn("tty1");
    tty_spawn("tty2");
    tty_spawn("tty3");
}
