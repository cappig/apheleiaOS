#include <errno.h>
#include <sys/ioctl.h>
#include <termios.h>

int tcgetattr(int fd, struct termios* tos) {
    if (!tos) {
        errno = EINVAL;
        return -1;
    }

    return ioctl(fd, TCGETS, tos);
}

int tcsetattr(int fd, int optional_actions, const struct termios* tos) {
    if (!tos) {
        errno = EINVAL;
        return -1;
    }

    unsigned long request = 0;

    if (optional_actions == TCSANOW)
        request = TCSETS;
    else if (optional_actions == TCSADRAIN)
        request = TCSETSW;
    else if (optional_actions == TCSAFLUSH)
        request = TCSETSF;
    else {
        errno = EINVAL;
        return -1;
    }

    return ioctl(fd, request, (void*)tos);
}

speed_t cfgetispeed(const struct termios* tos) {
    if (!tos) {
        errno = EINVAL;
        return 0;
    }

    return tos->c_ispeed;
}

speed_t cfgetospeed(const struct termios* tos) {
    if (!tos) {
        errno = EINVAL;
        return 0;
    }

    return tos->c_ospeed;
}

int cfsetispeed(struct termios* tos, speed_t speed) {
    if (!tos) {
        errno = EINVAL;
        return -1;
    }

    tos->c_ispeed = speed;
    return 0;
}

int cfsetospeed(struct termios* tos, speed_t speed) {
    if (!tos) {
        errno = EINVAL;
        return -1;
    }

    tos->c_ospeed = speed;
    return 0;
}

void cfmakeraw(struct termios* tos) {
    if (!tos)
        return;

    tos->c_iflag &= (tcflag_t) ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    tos->c_oflag &= (tcflag_t) ~OPOST;
    tos->c_lflag &= (tcflag_t) ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tos->c_cflag &= (tcflag_t) ~(CSIZE | PARENB);
    tos->c_cflag |= CS8;
    tos->c_cc[VMIN] = 1;
    tos->c_cc[VTIME] = 0;
}
