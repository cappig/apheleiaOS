#pragma once

#define ATTRIBUTE(name) __attribute__((name))

#define NORETURN         __attribute__((noreturn))
#define PACKED           __attribute__((packed))
#define ALIGNED(align)   __attribute__((aligned(align)))
#define SECTION(section) __attribute__((section(section)))
#define UNUSED           __attribute__((unused))
#define NONNULL          __attribute__((nonnull))
#define FALLTHROUGH      __attribute__((fallthrough))
#define NONSTRING        __attribute__((nonstring))
