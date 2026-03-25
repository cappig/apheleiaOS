#include <setjmp.h>

int setjmp(jmp_buf env) {
    return arch_setjmp(env);
}

void longjmp(jmp_buf env, int val) {
    arch_longjmp(env, val);
}
