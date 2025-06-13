
extern void __init_stdio_buffers(void);

void __libc_init(void) {
    __init_stdio_buffers();
}
