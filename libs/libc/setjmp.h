#pragma once

#include <arch/setjmp.h>

typedef arch_jmp_buf_t jmp_buf;

int setjmp(jmp_buf env);
__attribute__((noreturn)) void longjmp(jmp_buf env, int val);
