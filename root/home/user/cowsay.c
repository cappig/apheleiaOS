#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_MESSAGE "hello from apheleiaOS"
#define BUBBLE_WIDTH    60

static void print_line(char left, char mid, char right, size_t width) {
    putchar(left);

    for (size_t i = 0; i < width + 2; i++) {
        putchar(mid);
    }

    putchar(right);
    putchar('\n');
}

static char *make_message(int argc, char **argv) {
    if (argc <= 1) {
        char *msg = malloc(strlen(DEFAULT_MESSAGE) + 1);
        if (msg) {
            strcpy(msg, DEFAULT_MESSAGE);
        }

        return msg;
    }

    size_t len = 0;

    for (int i = 1; i < argc; i++) {
        if (i > 1) {
            len++;
        }

        len += strlen(argv[i]);
    }

    char *msg = malloc(len + 1);
    if (!msg) {
        return NULL;
    }

    char *out = msg;

    for (int i = 1; i < argc; i++) {
        if (i > 1) {
            *out++ = ' ';
        }

        size_t arg_len = strlen(argv[i]);
        memcpy(out, argv[i], arg_len);
        out += arg_len;
    }

    *out = '\0';
    return msg;
}

static size_t next_piece(const char *msg, size_t pos, size_t len, size_t *start_out, size_t *next_out) {
    while (pos < len && msg[pos] == ' ') {
        pos++;
    }

    if (pos >= len) {
        *start_out = pos;
        *next_out = pos;
        return 0;
    }

    *start_out = pos;

    size_t left = len - pos;
    size_t take = left < BUBBLE_WIDTH ? left : BUBBLE_WIDTH;

    if (left > BUBBLE_WIDTH) {
        size_t split = pos + take;

        for (size_t i = pos + take; i > pos; i--) {
            if (msg[i - 1] == ' ') {
                split = i - 1;
                break;
            }
        }

        if (split > pos) {
            take = split - pos;
        }
    }

    *next_out = pos + take;
    return take;
}

static size_t wrapped_width(const char *msg) {
    size_t len = strlen(msg);
    size_t pos = 0;
    size_t width = 0;

    while (pos < len) {
        size_t start = 0;
        size_t next = 0;
        size_t take = next_piece(msg, pos, len, &start, &next);

        if (take > width) {
            width = take;
        }

        pos = next;
    }

    return width ? width : 1;
}

static void print_wrapped(const char *msg, size_t width) {
    size_t len = strlen(msg);
    size_t pos = 0;

    if (!len) {
        fputs("<   >\n", stdout);
        return;
    }

    while (pos < len) {
        size_t start = 0;
        size_t next = 0;
        size_t take = next_piece(msg, pos, len, &start, &next);

        fputs("< ", stdout);
        fwrite(msg + start, 1, take, stdout);

        for (size_t i = take; i < width; i++) {
            putchar(' ');
        }

        fputs(" >\n", stdout);
        pos = next;
    }
}

int main(int argc, char **argv) {
    char *msg = make_message(argc, argv);
    if (!msg) {
        return 1;
    }

    size_t width = wrapped_width(msg);

    print_line(' ', '_', ' ', width);

    print_wrapped(msg, width);

    print_line(' ', '-', ' ', width);

    puts("        \\   ^__^");
    puts("         \\  (oo)\\_______");
    puts("            (__)\\       )\\/\\");
    puts("                ||----w |");
    puts("                ||     ||");

    free(msg);
    return 0;
}
