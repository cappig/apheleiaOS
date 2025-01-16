#include "tty.h"

#include <base/addr.h>
#include <base/types.h>
#include <fs/ustar.h>
#include <gfx/vga.h>
#include <log/log.h>
#include <string.h>
#include <term/term.h>

#include "drivers/initrd.h"
#include "drivers/vesa.h"
#include "mem/heap.h"
#include "sys/console.h"
#include "sys/panic.h"
#include "sys/video.h"
#include "vfs/fs.h"
#include "vfs/pty.h"

// The current virtual tty is the one that gets all the user input
// and the one that gets drawn to the screen
isize current_tty = TTY_NONE;


static virtual_tty ttys[TTY_COUNT] = {0};

// All virtual ttys run in the same mode and have the same size
static u8 mode = TERM_RASTER;

static usize width = 0;
static usize height = 0;

static psf_font* font = NULL; // only for raster terminals


static bool _load_font(const char* name) {
    if (!name)
        return false;

    ustar_file* font_file = initrd_find(name);

    if (!font_file)
        return false;

    font = kcalloc(sizeof(psf_font));

    if (!psf_parse(font_file->data, font)) {
        kfree(font);
        font = NULL;

        return false;
    }

    return true;
}

static void _mount_tty(pseudo_tty* pty, usize index) {
    assert(index < 10);

    char name[] = "tty0";
    name[3] += index;

    pty->slave->name = strdup(name);

    tree_node* node = tree_create_node(pty->slave);
    vfs_mount("/dev", node);
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
    if (!buf || !len || !pty)
        return;

    gfx_terminal* gterm = pty->private;
    if (!gterm)
        return;

    terminal* term = gterm->term;
    if (!term)
        return;

    term_parse(term, buf, len);
}


void tty_input(usize index, u8* data, usize len) {
    virtual_tty* vtty = get_tty(index);

    if (!vtty)
        return;

    vfs_node* node = vtty->pty->master;
    node->interface->write(node, data, 0, len);
}

void tty_output(usize index, u8* data, usize len) {
    virtual_tty* vtty = get_tty(index);

    if (!vtty)
        return;

    vfs_node* node = vtty->pty->slave;
    node->interface->write(node, data, 0, len);
}


bool tty_set_current(usize index) {
    assert(index < TTY_COUNT);

    if (current_tty != TTY_NONE) {
        virtual_tty* old_vtty = &ttys[current_tty];

        gfx_terminal* old_gterm = old_vtty->gterm;
        old_gterm->draw = false;
    }

    virtual_tty* vtty = &ttys[index];

    gfx_terminal* gterm = vtty->gterm;
    gterm->draw = true;

    current_tty = index;

    pseudo_tty* pty = vtty->pty;
    log_debug("/dev/%s is now the current virtual tty", pty->slave->name);

    term_redraw(gterm->term);

    return true;
}

virtual_tty* get_tty(isize index) {
    if (index == TTY_NONE)
        return NULL;

    assert(index < TTY_COUNT);

    virtual_tty* vtty = &ttys[index];

    if (!vtty->pty)
        return NULL;

    return vtty;
}


virtual_tty* tty_spawn(usize index) {
    assert(index < TTY_COUNT);

    if (video.mode == GFX_NONE)
        return NULL;

    pseudo_tty* pty = pty_create(TTY_BUF_SIZE);

    if (!pty)
        return NULL;

    term_draw_fn draw_fn = (mode == TERM_RASTER) ? _draw_vesa : _draw_vga;
    gfx_terminal* gterm = gfx_term_init(width, height, mode, font, draw_fn);

    if (!gterm)
        return NULL;

    gterm->draw = false;

    pty->out_hook = _pty_write;
    pty->private = gterm;

    _mount_tty(pty, index);

    virtual_tty* vtty = &ttys[index];

    vtty->pty = pty;
    vtty->gterm = gterm;

    return vtty;
}


void tty_init(boot_handoff* handoff) {
    graphics_state* gfx_state = &handoff->graphics;

    if (gfx_state->mode == GFX_NONE)
        return;

    if (gfx_state->mode == GFX_VESA) {
        if (!_load_font(INITRD_FONT_NAME))
            log_warn("Failed to load tty psf font!");

        width = gfx_state->width;
        height = gfx_state->height;

        mode = TERM_RASTER;
    } else {
        width = VGA_WIDTH;
        height = VGA_HEIGHT;

        mode = TERM_VGA;
    }
}

void tty_spawn_devs() {
    if (video.mode == GFX_NONE)
        return;

    for (usize i = 0; i < TTY_COUNT; i++)
        tty_spawn(i);

    console_set_tty(TTY_CONSOLE);
    tty_set_current(TTY_CONSOLE);
}
