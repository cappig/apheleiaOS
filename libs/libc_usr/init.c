extern void __init_stdio_buffers(void);

char** environ;


void __libc_init(int argc, char** argv, char** envp) {
    environ = envp;

    __init_stdio_buffers();
}
