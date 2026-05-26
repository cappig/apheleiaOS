#pragma once

#include <arch/setjmp.h>

typedef arch_jmp_buf_t jmp_buf;

#ifndef APHELEIA_SETJMP_NO_MACRO
#define setjmp(env) arch_setjmp(env)
#else
__attribute__((returns_twice)) int setjmp(jmp_buf env);
#endif

__attribute__((noreturn)) void longjmp(jmp_buf env, int val);
