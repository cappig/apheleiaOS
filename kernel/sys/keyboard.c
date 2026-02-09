#include "keyboard.h"

#include <data/ring.h>
#include <data/vector.h>
#include <log/log.h>
#include <sched/scheduler.h>
#include <stdlib.h>
#include <string.h>
#include <sys/tty.h>
#include <sys/tty_input.h>
#include <x86/asm.h>

static vector_t* kbds = NULL;
static ring_buffer_t* buffer = NULL;
static sched_wait_queue_t kbd_wait = {0};

static bool _vec_push_ptr(vector_t* vec, void* ptr) {
    return vec_push(vec, &ptr);
}

static keyboard_dev_t* _get(size_t index) {
    keyboard_dev_t** slot = vec_at(kbds, index);
    if (!slot)
        return NULL;
    return *slot;
}

static char* _strdup(const char* src) {
    if (!src)
        return NULL;

    size_t len = strlen(src);
    char* out = malloc(len + 1);

    if (!out)
        return NULL;

    memcpy(out, src, len);
    out[len] = '\0';
    return out;
}

static void _update_modifiers(keyboard_dev_t* kbd, bool action, u8 code) {
    if (code == KBD_LEFT_SHIFT || code == KBD_RIGHT_SHIFT)
        kbd->shift = (action == KEY_DOWN);

    if (code == KBD_LEFT_CTRL || code == KBD_RIGHT_CTRL)
        kbd->ctrl = (action == KEY_DOWN);

    if (code == KBD_LEFT_ALT || code == KBD_RIGHT_ALT)
        kbd->alt = (action == KEY_DOWN);

    if ((code == KBD_CAPSLOCK) && (action == KEY_DOWN))
        kbd->capslock = !kbd->capslock;
}

ssize_t keyboard_read(vfs_node_t* node, void* buf, size_t offset, size_t len, u32 flags) {
    (void)node;
    (void)offset;
    (void)flags;

    if (!buf || !buffer || !len)
        return -1;

    for (;;) {
        unsigned long irq_flags = irq_save();
        size_t popped = ring_buffer_pop_array(buffer, buf, len);
        irq_restore(irq_flags);

        if (popped)
            return (ssize_t)popped;

        if (!sched_is_running())
            continue;

        sched_block(&kbd_wait);
    }
}

void keyboard_handle_key(key_event event) {
    if (!kbds || !buffer)
        return;

    keyboard_dev_t* kbd = _get(event.source);

    if (!kbd) {
        log_warn("keyboard: input from unknown source");
        return;
    }

    bool action = (event.type & KEY_ACTION) != 0;
    ring_buffer_push_array(buffer, (u8*)&event, sizeof(event));
    sched_wake_all(&kbd_wait);

    keyboard_update_modifiers(kbd, action, event.code);

    if (!action)
        return;

    if (kbd->alt) {
        if (event.code == KBD_F1) {
            tty_set_current(TTY_CONSOLE);
            return;
        }

        if (event.code >= KBD_F2 && event.code <= (KBD_F2 + TTY_COUNT - 1)) {
            size_t index = (size_t)(event.code - KBD_F2);
            if (index < TTY_COUNT)
                tty_set_current(TTY_USER_TO_SCREEN(index));
            return;
        }
    }

    bool is_alpha = (event.code >= KBD_A && event.code <= KBD_Z);
    bool shift = kbd->shift ^ (kbd->capslock && is_alpha);
    char ch = kbd_to_ascii(event, kbd->keymap, shift);

    if (!ch)
        return;

    if (kbd->ctrl) {
        if (ch >= 'a' && ch <= 'z')
            ch = (char)(ch - 'a' + 1);
        else if (ch >= 'A' && ch <= 'Z')
            ch = (char)(ch - 'A' + 1);
    }

    tty_input_push(ch);
}

u8 keyboard_register(const char* name, ascii_keymap* keymap) {
    if (!kbds || !buffer)
        keyboard_init();

    if (!kbds || !buffer)
        return 0;

    keyboard_dev_t* kbd = calloc(1, sizeof(keyboard_dev_t));

    if (!kbd)
        return 0;

    kbd->name = _strdup(name);
    kbd->keymap = keymap ? keymap : &us_keymap;

    if (!_vec_push_ptr(kbds, kbd)) {
        free((void*)kbd->name);
        free(kbd);
        return 0;
    }

    log_info("keyboard: registered %s", kbd->name ? kbd->name : "device");
    return (u8)(kbds->size - 1);
}

bool keyboard_init(void) {
    if (!kbds)
        kbds = vec_create(sizeof(keyboard_dev_t*));

    if (!kbds)
        return false;

    if (!buffer)
        buffer = ring_buffer_create(KBD_DEV_BUFFER_SIZE);

    if (!buffer)
        return false;

    sched_wait_queue_init(&kbd_wait);
    return true;
}
