#include <io.h>
#include <unistd.h>

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        io_write_str(argv[i]);

        if (i + 1 < argc)
            io_write_str(" ");
    }

    io_write_str("\n");
    return 0;
}
