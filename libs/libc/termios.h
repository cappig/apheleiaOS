#pragma once

// Defines strures related to the termios POSIX interface
// https://pubs.opengroup.org/onlinepubs/7908799/xsh/termios.h.html
// https://www.man7.org/linux/man-pages/man3/termios.3.html

#define NCCS 32

typedef unsigned char cc_t;
typedef unsigned int speed_t;
typedef unsigned int tcflag_t;

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t c_cc[NCCS];

    speed_t c_ispeed;
    speed_t c_ospeed;
};

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

#ifdef EXTEND_LIBC
typedef struct termios termios_t;
typedef struct winsize winsize_t;
#endif

// https://www.gnu.org/software/libc/manual/html_node/Editing-Characters.html
enum termios_cc {
    VEOF, // ^D, canonical only
    VEOL, // null, canonical only
    VERASE, // ^H, canonical only
    VINTR, // ^C, all modes
    VKILL, // ^U, canonical only
    VMIN, // minimum number of characters for raw read
    VQUIT, // ^\, all modes
    VSTART, // ^Q, all modes
    VSTOP, // ^S, all modes
    VSUSP, // ^Z, all modes
    VTIME, // timeout in deciseconds for raw read
    VWERASE, // ^W, canonical only
    VLNEXT, // ^V, all modes
    VREPRINT, // ^R, canonical only
};

enum termios_iflags {
    BRKINT = 1 << 0,
    ICRNL = 1 << 1,
    IGNBRK = 1 << 2,
    IGNCR = 1 << 3,
    IGNPAR = 1 << 4,
    INLCR = 1 << 5,
    INPCK = 1 << 6,
    ISTRIP = 1 << 7,
    IUCLC = 1 << 8,
    IXANY = 1 << 9,
    IXOFF = 1 << 10,
    IXON = 1 << 11,
    PARMRK = 1 << 12,
};

enum termios_oflags {
    OPOST = 1 << 0,
    ONLCR = 1 << 1,
    OCRNL = 1 << 2,
    ONOCR = 1 << 3,
    ONLRET = 1 << 4,
    OFILL = 1 << 5,

    NLDLY = 1 << 6,
    NL0 = 0,
    NL1 = 1,

    CRDLY = 1 << 7 | 1 << 8,
    CR0 = 0 << 7,
    CR1 = 1 << 7,
    CR2 = 2 << 7,
    CR3 = 3 << 7,

    TABDLY = 1 << 9 | 1 << 10,
    TAB0 = 0 << 9,
    TAB1 = 1 << 9,
    TAB2 = 2 << 9,
    TAB3 = 3 << 9,

    BSDLY = 1 << 11,
    BS0 = 0 << 11,
    BS1 = 1 << 11,

    VTDLY = 1 << 12,
    VT0 = 0 << 12,
    VT1 = 1 << 12,

    FFDLY = 1 << 13,
    FF0 = 0 << 13,
    FF1 = 1 << 13,
};

enum termios_cflags {
    CSIZE = 1 << 0 | 1 << 1,
    CS5 = 0 << 0,
    CS6 = 1 << 0,
    CS7 = 2 << 0,
    CS8 = 3 << 0,

    CSTOPB = 1 << 2,
    CREAD = 1 << 3,
    PARENB = 1 << 4,
    PARODD = 1 << 5,
    HUPCL = 1 << 6,
    CLOCAL = 1 << 7,
    CMSPAR = 1 << 8,
};

enum termios_lflags {
    ECHO = 1 << 0,
    ECHOE = 1 << 1,
    ECHOK = 1 << 2,
    ECHONL = 1 << 3,
    ECHOCTL = 1 << 4,
    ICANON = 1 << 5,
    IEXTEN = 1 << 6,
    ISIG = 1 << 7,
    NOFLSH = 1 << 8,
    TOSTOP = 1 << 9,
};

enum termios_baud {
    B0 = 0, // hang up
    B50 = 50,
    B75 = 75,
    B110 = 110,
    B134 = 134,
    B150 = 150,
    B200 = 200,
    B300 = 300,
    B600 = 600,
    B1200 = 1200,
    B1800 = 1800,
    B2400 = 2400,
    B4800 = 4800,
    B9600 = 9600, // traditional default
    B19200 = 19200,
    B38400 = 38400,
    B57600 = 57600,
    B115200 = 115200,
};


// Some sane defaults for termios to use
#ifdef EXTEND_LIBC
inline termios_t* __termios_default_init(termios_t* tos) {
    if (!tos)
        return tos;

    tos->c_cc[VEOF] = 'D' - 0x40;
    tos->c_cc[VEOL] = 0;
    tos->c_cc[VERASE] = 'H' - 0x40;
    tos->c_cc[VINTR] = 'C' - 0x40;
    tos->c_cc[VKILL] = 'U' - 0x40;
    tos->c_cc[VQUIT] = '\\' - 0x40;
    tos->c_cc[VSTART] = 'Q' - 0x40;
    tos->c_cc[VSTOP] = 'S' - 0x40;
    tos->c_cc[VSUSP] = 'Z' - 0x40;
    tos->c_cc[VWERASE] = 'W' - 0x40;
    tos->c_cc[VLNEXT] = 'V' - 0x40;
    tos->c_cc[VREPRINT] = 'R' - 0x40;
    tos->c_cc[VTIME] = 0;
    tos->c_cc[VMIN] = 1;

    tos->c_iflag = BRKINT | ICRNL;
    tos->c_oflag = OPOST;
    tos->c_cflag = CS8 | CREAD | HUPCL;
    tos->c_lflag = ECHO | ECHOE | ECHOK | ECHOCTL | ICANON | IEXTEN | ISIG;

    tos->c_ispeed = B115200;
    tos->c_ospeed = B115200;

    return tos;
}
#endif
