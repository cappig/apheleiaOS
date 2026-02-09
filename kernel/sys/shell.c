#include "shell.h"

#include <data/list.h>
#include <data/tree.h>
#include <log/log.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/tty.h>
#include <sys/vfs.h>

#define SHELL_LINE_MAX 256
#define SHELL_ARG_MAX  16

static const tty_handle_t shell_tty = {
    .kind = TTY_HANDLE_NAMED,
    .index = 0,
};

static char shell_cwd[128] = "/";

static void _write(const char* s) {
    if (!s)
        return;

    tty_write_handle(&shell_tty, s, strlen(s));
}

static void _write_len(const char* s, size_t len) {
    if (!s || !len)
        return;

    tty_write_handle(&shell_tty, s, len);
}

static void _printf(const char* fmt, ...) {
    char buf[256];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    _write(buf);
}

static char _getc(void) {
    char ch = 0;
    ssize_t read = tty_read_handle(&shell_tty, &ch, 1);

    if (read <= 0)
        return 0;

    return ch;
}

static size_t _read_line(char* out, size_t len) {
    if (!out || !len)
        return 0;

    size_t pos = 0;

    for (;;) {
        char ch = _getc();

        if (!ch)
            continue;

        if (ch == '\r')
            ch = '\n';

        if (ch == '\n') {
            _write("\n");
            out[pos] = '\0';
            return pos;
        }

        if (ch == '\b' || ch == 0x7f) {
            if (pos > 0) {
                pos--;
                _write("\b \b");
            }
            continue;
        }

        if ((unsigned char)ch < 0x20 || (unsigned char)ch > 0x7e)
            continue;

        if (pos + 1 >= len)
            continue;

        out[pos++] = ch;
        _write_len(&ch, 1);
    }
}

static bool _resolve_path(const char* path, char* out, size_t out_len) {
    if (!path || !out || !out_len)
        return false;

    if (path[0] == '/') {
        snprintf(out, out_len, "%s", path);
        return true;
    }

    if (!strcmp(shell_cwd, "/"))
        snprintf(out, out_len, "/%s", path);
    else
        snprintf(out, out_len, "%s/%s", shell_cwd, path);

    return true;
}

static void _cmd_help(void) {
    _write("commands: help ls cat cd pwd echo clear\n");
}

static void _cmd_pwd(void) {
    _printf("%s\n", shell_cwd);
}

static void _cmd_clear(void) {
    _write("\x1b[2J\x1b[H");
}

static void _cmd_echo(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        _write(argv[i]);
        if (i + 1 < argc)
            _write(" ");
    }
    _write("\n");
}

static void _cmd_ls(int argc, char** argv) {
    const char* path = (argc > 1) ? argv[1] : shell_cwd;
    char full[256];

    _resolve_path(path, full, sizeof(full));

    vfs_node_t* node = vfs_lookup(full);
    if (!node) {
        _printf("ls: %s: not found\n", path);
        return;
    }

    if (VFS_IS_LINK(node->type) && node->link)
        node = node->link;

    if (node->type != VFS_DIR) {
        _printf("%s\n", node->name ? node->name : full);
        return;
    }

    tree_node_t* tnode = node->tree_entry;
    if (!tnode || !tnode->children) {
        _write("\n");
        return;
    }

    ll_foreach(child, tnode->children) {
        tree_node_t* cnode = child->data;
        vfs_node_t* vnode = cnode ? cnode->data : NULL;
        if (!vnode || !vnode->name)
            continue;

        _write(vnode->name);
        if (vnode->type == VFS_DIR || vnode->type == VFS_MOUNT)
            _write("/");
        _write("\n");
    }
}

static void _cmd_cat(int argc, char** argv) {
    if (argc < 2) {
        _write("cat: missing operand\n");
        return;
    }

    char full[256];
    _resolve_path(argv[1], full, sizeof(full));

    vfs_node_t* node = vfs_lookup(full);
    if (!node || node->type != VFS_FILE) {
        _printf("cat: %s: not a file\n", argv[1]);
        return;
    }

    size_t offset = 0;
    u8 buffer[256];

    while (offset < node->size) {
        size_t remaining = (size_t)(node->size - offset);
        size_t to_read = remaining > sizeof(buffer) ? sizeof(buffer) : remaining;
        ssize_t read = vfs_read(node, buffer, offset, to_read, 0);

        if (read <= 0)
            break;

        _write_len((const char*)buffer, (size_t)read);
        offset += (size_t)read;
    }

    _write("\n");
}

static void _cmd_cd(int argc, char** argv) {
    const char* path = (argc > 1) ? argv[1] : "/";
    char full[256];

    _resolve_path(path, full, sizeof(full));

    vfs_node_t* node = vfs_lookup(full);
    if (!node) {
        _printf("cd: %s: not found\n", path);
        return;
    }

    if (VFS_IS_LINK(node->type) && node->link)
        node = node->link;

    if (node->type != VFS_DIR) {
        _printf("cd: %s: not a directory\n", path);
        return;
    }

    snprintf(shell_cwd, sizeof(shell_cwd), "%s", full);
}

static int _split(char* line, char** argv, int max) {
    int argc = 0;
    char* save = NULL;
    char* tok = strtok_r(line, " \t", &save);

    while (tok && argc < max) {
        argv[argc++] = tok;
        tok = strtok_r(NULL, " \t", &save);
    }

    return argc;
}

static void _exec(char* line) {
    char* argv[SHELL_ARG_MAX];
    int argc = _split(line, argv, SHELL_ARG_MAX);

    if (argc <= 0)
        return;

    if (!strcmp(argv[0], "help")) {
        _cmd_help();
    } else if (!strcmp(argv[0], "ls")) {
        _cmd_ls(argc, argv);
    } else if (!strcmp(argv[0], "cat")) {
        _cmd_cat(argc, argv);
    } else if (!strcmp(argv[0], "cd")) {
        _cmd_cd(argc, argv);
    } else if (!strcmp(argv[0], "pwd")) {
        _cmd_pwd();
    } else if (!strcmp(argv[0], "echo")) {
        _cmd_echo(argc, argv);
    } else if (!strcmp(argv[0], "clear")) {
        _cmd_clear();
    } else {
        _printf("sh: %s: command not found\n", argv[0]);
    }
}

void shell_main(void) {
    _write("apheleiaOS sh\n");

    for (;;) {
        _printf("%s$ ", shell_cwd);

        char line[SHELL_LINE_MAX];
        if (!_read_line(line, sizeof(line)))
            continue;

        _exec(line);
    }
}
