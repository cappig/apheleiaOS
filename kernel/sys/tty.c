#include "tty.h"

#include <base/addr.h>
#include <base/types.h>
#include <fs/ustar.h>
#include <gfx/vga.h>
#include <log/log.h>
#include <string.h>
#include <term/term.h>

#include "data/vector.h"
#include "drivers/ide.h"
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

static vector* ttys = NULL;

// All virtual ttys run in the same mode and have the same size
static u8 mode = TERM_RASTER;

static usize width = 0;
static usize height = 0;

static psf_font* font = NULL; // only for raster terminals


static bool _load_font(const char* name) {
    if (!name)
        return false;

    vfs_node* file = vfs_lookup(name);

    if (!file)
        return false;

    void* buffer = kmalloc(file->size);
    vfs_read(file, buffer, 0, file->size, 0);

    font = kcalloc(sizeof(psf_font));

    if (!psf_parse(buffer, font)) {
        kfree(font);

        font = NULL;
        return false;
    }

    return true;
}

static void _mount_tty(pseudo_tty* pty, usize index) {
    // TODO: allow for more than 10 ttys!!! >:|
    assert(index < 10);

    char name[] = "tty0";
    name[3] += index;

    pty->slave->name = strdup(name);

    vfs_node* dev = vfs_open("/dev", VFS_DIR, true, KDIR_MODE);
    vfs_insert_child(dev, pty->slave);
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

    vfs_write(node, data, 0, len, 0);
}

void tty_output(usize index, u8* data, usize len) {
    virtual_tty* vtty = get_tty(index);

    if (!vtty)
        return;

    vfs_node* node = vtty->pty->slave;

    vfs_write(node, data, 0, len, 0);
}


bool tty_set_current(usize index) {
    assert(ttys);

    if (current_tty != TTY_NONE) {
        virtual_tty* old_vtty = get_tty(current_tty);

        if (old_vtty) {
            gfx_terminal* old_gterm = old_vtty->gterm;
            old_gterm->draw = false;
        }
    }

    virtual_tty* vtty = get_tty(index);

    if (!vtty)
        return false;

    gfx_terminal* gterm = vtty->gterm;
    gterm->draw = true;

    current_tty = index;

    pseudo_tty* pty = vtty->pty;
    log_debug("/dev/%s is now the current virtual tty", pty->slave->name);

    term_redraw(gterm->term);

    return true;
}

virtual_tty* get_tty(isize index) {
    if (index < 0)
        return NULL;

    assert(ttys);

    virtual_tty* vtty = vec_at(ttys, index);

    if (!vtty || !vtty->pty)
        return NULL;

    return vtty;
}


virtual_tty* tty_spawn() {
    assert(ttys);

    if (video.mode == GFX_NONE)
        return NULL;

    pseudo_tty* pty = pty_create(TTY_BUF_SIZE);

    pty->flags |= PTY_CANONICAL | PTY_ECHO;

    if (!pty)
        return NULL;

    term_draw_fn draw_fn = (mode == TERM_RASTER) ? _draw_vesa : _draw_vga;
    gfx_terminal* gterm = gfx_term_init(width, height, mode, font, draw_fn);

    if (!gterm)
        return NULL;

    gterm->draw = false;

    pty->out_hook = _pty_write;
    pty->private = gterm;

    virtual_tty* vtty = kmalloc(sizeof(virtual_tty));

    vtty->id = ttys->size;
    vtty->pty = pty;
    vtty->gterm = gterm;

    _mount_tty(pty, vtty->id);

    vec_push(ttys, vtty);

    kfree(vtty);

    return vec_at(ttys, vtty->id);
}


void tty_init(boot_handoff* handoff) {
    graphics_state* gfx_state = &handoff->graphics;

    ttys = vec_create_sized(TTY_COUNT, sizeof(virtual_tty));

    if (gfx_state->mode == GFX_NONE)
        return;

    if (gfx_state->mode == GFX_VESA) {
        if (!_load_font("/boot/font.psf"))
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
    assert(ttys);

    if (video.mode == GFX_NONE)
        return;

    for (usize i = 0; i < TTY_COUNT; i++)
        tty_spawn();

    console_set_tty(TTY_CONSOLE);
    tty_set_current(TTY_CONSOLE);
}
