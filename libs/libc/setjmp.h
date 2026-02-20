#pragma once

#if defined(__x86_64__)
typedef unsigned long jmp_buf[8];
#elif defined(__i386__)
typedef unsigned int jmp_buf[6];
#else
#error "Unsupported architecture for setjmp/longjmp"
#endif

int setjmp(jmp_buf env);
__attribute__((noreturn)) void longjmp(jmp_buf env, int val);
