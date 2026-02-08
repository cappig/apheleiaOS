#include <log/log.h>
#include <sys/panic.h>
#include <x86/asm.h>
#include <x86/boot.h>
#include <x86/gdt.h>
#include <x86/idt.h>
#include <x86/mm/heap.h>
#include <x86/mm/physical.h>
#include <x86/pic.h>
#include <x86/serial.h>

static void _serial_puts(const char* s) {
    send_serial_string(SERIAL_COM1, s);
}

static void _select_log_level(const boot_info_t* info) {
    if (!info)
        return;

    switch (info->args.debug) {
    case DEBUG_NONE:
        log_set_lvl(LOG_INFO);
        break;
    case DEBUG_MINIMAL:
        log_set_lvl(LOG_INFO);
        break;
    case DEBUG_ALL:
        log_set_lvl(LOG_DEBUG);
        break;
    default:
        break;
    }
}

static uintptr_t _read_stack_ptr(void) {
    uintptr_t sp = 0;

#if defined(__x86_64__)
    asm volatile("mov %%rsp, %0" : "=r"(sp));
#else
    asm volatile("mov %%esp, %0" : "=r"(sp));
#endif

    return sp;
}

void arch_init(void* boot_info) {
    boot_info_t* info = boot_info;

    init_serial(SERIAL_COM1, SERAIL_DEFAULT_LINE, SERIAL_DEFAULT_BAUD);
    log_init(serial_puts);
    asm volatile("cld");

    if (!info)
        panic("boot info missing");

    select_log_level(info);

#if defined(__x86_64__)
    log_info("apheleiaOS kernel (x86_64) booting");
#else
    log_info("apheleiaOS kernel (x86_32) booting");
#endif

    log_debug("initializing GDT");
    gdt_init();
    log_debug("initializing TSS");
    tss_init(_read_stack_ptr());
    log_debug("initializing PIC");
    pic_init();
    log_debug("initializing IDT");
    idt_init();
    log_debug("initializing physical memory");
    log_debug("initializing PMM");
    pmm_init(&info->memory_map);
    log_debug("PMM ready");
    log_debug("initializing heap");
    heap_init();
    log_debug("heap ready");
    log_debug("initializing malloc");
    init_malloc();
    log_debug("malloc ready");

#if defined(__x86_64__)
    log_info("apheleiaOS kernel (x86_64) booted");
#else
    log_info("apheleiaOS kernel (x86_32) booted");
#endif
}
