#include "keyboard.h"

#include <base/attributes.h>
#include <ctype.h>
#include <data/ring.h>
#include <data/vector.h>
#include <input/kbd.h>
#include <input/keymap.h>
#include <log/log.h>
#include <string.h>

#include "arch/panic.h"
#include "mem/heap.h"
#include "sys/tty.h"
#include "vfs/fs.h"


static vector* kbds = NULL;

// All the keyboards send their input to a single buffer
static ring_buffer* buffer;

static isize _read(UNUSED vfs_node* node, void* buf, UNUSED usize offset, usize len) {
    if (!buf)
        return -1;

    bool popped = ring_buffer_pop_array(buffer, buf, len);

    return popped ? len : 0;
}

// Keep track of modifier keys and latch capslock
static void _update_modifiers(keyboard* kbd, bool action, u8 code) {
    if (code == KBD_LEFT_SHIFT || code == KBD_RIGHT_SHIFT)
        kbd->shift = (action == KEY_DOWN);

    if (code == KBD_LEFT_CTRL || code == KBD_RIGHT_CTRL)
        kbd->ctrl = (action == KEY_DOWN);

    if (code == KBD_LEFT_ALT || code == KBD_RIGHT_ALT)
        kbd->alt = (action == KEY_DOWN);

    if ((code == KBD_CAPSLOCK) && (action == KEY_DOWN))
        kbd->capslock = !kbd->capslock;
}

static bool _send_to_tty(keyboard* kbd, key_event event) {
    u8 ch = kbd_to_ascii(event, kbd->keymap, kbd->shift);

    if (!ch)
        return false;

    if (current_tty == TTY_NONE)
        return false;

    if (kbd->capslock) {
        if (kbd->shift)
            ch = tolower(ch);
        else
            ch = toupper(ch);
    }

    // Send the ascii key to the current virtual tty
    tty_input(current_tty, &ch, 1);

    return true;
}

// We can switch between virtual ttys here
// Holding down <Alt>(<Fn>)<F#> will set the surrent tty to #
// depending on the keyboard Fn may or may not be needed
static bool _switch_tty(keyboard* kbd, key_event event) {
    if (!kbd->alt)
        return false;

    if (event.code < KBD_F1 || event.code > KBD_F12)
        return false;

    usize index = event.code - KBD_F1;

    if (index >= TTY_COUNT)
        return false;

    tty_set_current(index);

    return true;
}

// All keyboard input should be sent here by drivers
void kbd_handle_key(key_event event) {
    bool action = event.type & KEY_ACTION;
    u8 code = event.code;

    keyboard* kbd = vec_at(kbds, event.source);

    if (!kbd) {
        log_error("Keyboard got input from unknown source!");
        return;
    }

#ifdef INPUT_DEBUG
    log_debug("[INPUT_DEBUG] keyboard #%u, code = %u", event.source, event.code);
#endif

    ring_buffer_push_array(buffer, (u8*)&event, sizeof(key_event));

    _update_modifiers(kbd, action, code);

    if (action == KEY_UP)
        return;

    // Send the key to the current virtual tty
    _send_to_tty(kbd, event);

    // Switch the virtual tty if the right keys are being held
    _switch_tty(kbd, event);
}


u8 register_keyboard(char* name, ascii_keymap* keymap) {
    // We don't want a keyboard device with no keyboard connected,
    // so we init if at least one device gets registered
    if (!kbds)
        keyboard_init();

    keyboard* kbd = kcalloc(sizeof(keyboard));

    kbd->name = strdup(name);
    kbd->keymap = keymap ? keymap : &us_keymap;

    vec_push(kbds, kbd);

    log_info("Keyboard input device registered: %s", name);

    return kbds->size - 1;
}


bool keyboard_init() {
    // Since this runs after the drivers
    vfs_node* dev = vfs_create_node("kbd", VFS_CHARDEV);
    dev->interface = vfs_create_file_interface(_read, NULL);

    vfs_mount("/dev", tree_create_node(dev));

    kbds = vec_create(sizeof(keyboard));

    buffer = ring_buffer_create(KBD_DEV_BUFFER_SIZE);

    return true;
}
