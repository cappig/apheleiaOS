#pragma once

#include "libc/signal.h"


sighandler_t signal(int signum, sighandler_t handler);
