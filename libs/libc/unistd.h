#pragma once

#include <posix.h>

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

#define _SC_ARG_MAX         0
#define _SC_CHILD_MAX       1
#define _SC_CLK_TCK         2
#define _SC_OPEN_MAX        3
#define _SC_PAGESIZE        4
#define _SC_NPROCESSORS_CONF 5
#define _SC_NPROCESSORS_ONLN 6

#ifndef _KERNEL
#include <libc_usr/unistd.h>
#endif
