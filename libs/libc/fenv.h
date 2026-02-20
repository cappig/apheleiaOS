#pragma once

typedef int fenv_t;
typedef int fexcept_t;

#define FE_DIVBYZERO 1
#define FE_INEXACT   2
#define FE_INVALID   4
#define FE_OVERFLOW  8
#define FE_UNDERFLOW 16
#define FE_ALL_EXCEPT (FE_DIVBYZERO | FE_INEXACT | FE_INVALID | FE_OVERFLOW | FE_UNDERFLOW)
