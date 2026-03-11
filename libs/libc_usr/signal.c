#include <arch/sys.h>
#include <apheleia/syscall.h>
#include <fcntl.h>
#include <errno.h>
#include <libc_usr/signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/proc.h>
#include <unistd.h>

static void signal_trampoline(void) {
    syscall0(SYS_SIGRETURN);

    for (;;) {
        ;
    }
}

static struct sigaction signal_actions[NSIG];

static int _sig_valid(int signum) {
    return signum > 0 && signum < NSIG;
}

int sigemptyset(sigset_t *set) {
    if (!set) {
        errno = EINVAL;
        return -1;
    }

    *set = 0;
    return 0;
}

int sigfillset(sigset_t *set) {
    if (!set) {
        errno = EINVAL;
        return -1;
    }

    sigset_t full = 0;
    for (int sig = 1; sig < NSIG; sig++) {
        full |= (sigset_t)(1u << (sig - 1));
    }

    *set = full;
    return 0;
}

int sigaddset(sigset_t *set, int signum) {
    if (!set || !_sig_valid(signum)) {
        errno = EINVAL;
        return -1;
    }

    *set |= (sigset_t)(1u << (signum - 1));
    return 0;
}

int sigdelset(sigset_t *set, int signum) {
    if (!set || !_sig_valid(signum)) {
        errno = EINVAL;
        return -1;
    }

    *set &= (sigset_t) ~(1u << (signum - 1));
    return 0;
}

int sigismember(const sigset_t *set, int signum) {
    if (!set || !_sig_valid(signum)) {
        errno = EINVAL;
        return -1;
    }

    return ((*set & (sigset_t)(1u << (signum - 1))) != 0) ? 1 : 0;
}

sighandler_t signal(int signum, sighandler_t handler) {
    if (!_sig_valid(signum)) {
        errno = EINVAL;
        return SIG_ERR;
    }

    long ret = syscall3(
        SYS_SIGNAL,
        (uintptr_t)signum,
        (uintptr_t)handler,
        (uintptr_t)signal_trampoline
    );

    if (ret < 0) {
        errno = (int)-ret;
        return SIG_ERR;
    }

    signal_actions[signum].sa_handler = handler;
    signal_actions[signum].sa_flags = 0;
    signal_actions[signum].sa_mask = 0;
    signal_actions[signum].sa_restorer = NULL;

    return (sighandler_t)ret;
}

int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact) {
    if (!_sig_valid(signum)) {
        errno = EINVAL;
        return -1;
    }

    if (oldact) {
        memset(oldact, 0, sizeof(*oldact));
        oldact->sa_handler = signal_actions[signum].sa_handler;
        oldact->sa_mask = signal_actions[signum].sa_mask;
        oldact->sa_flags = signal_actions[signum].sa_flags;
        oldact->sa_restorer = signal_actions[signum].sa_restorer;
    }

    if (!act) {
        return 0;
    }

    sighandler_t next = act->sa_handler ? act->sa_handler : SIG_DFL;
    if (signal(signum, next) == SIG_ERR) {
        return -1;
    }

    signal_actions[signum] = *act;
    return 0;
}

static int _read_sigmask(sigset_t *mask_out) {
    if (!mask_out) {
        errno = EINVAL;
        return -1;
    }

    int fd = open("/proc/self/sigmask", O_RDONLY, 0);
    if (fd < 0) {
        return -1;
    }

    char text[64];
    ssize_t n = read(fd, text, sizeof(text) - 1);
    int saved_errno = errno;
    close(fd);
    errno = saved_errno;

    if (n < 0) {
        return -1;
    }

    text[n] = '\0';

    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (end == text) {
        errno = EIO;
        return -1;
    }

    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') {
        end++;
    }
    if (*end != '\0') {
        errno = EIO;
        return -1;
    }

    *mask_out = (sigset_t)value;
    return 0;
}

static int _write_sigmask(sigset_t mask) {
    int fd = open("/proc/self/sigmask", O_WRONLY, 0);
    if (fd < 0) {
        return -1;
    }

    char text[32];
    int text_len = snprintf(text, sizeof(text), "%u\n", (unsigned int)mask);
    if (text_len <= 0 || (size_t)text_len >= sizeof(text)) {
        close(fd);
        errno = EIO;
        return -1;
    }

    size_t written = 0;
    while (written < (size_t)text_len) {
        ssize_t rc = write(fd, text + written, (size_t)text_len - written);
        if (rc < 0) {
            int saved_errno = errno;
            close(fd);
            errno = saved_errno;
            return -1;
        }

        if (rc == 0) {
            close(fd);
            errno = EIO;
            return -1;
        }

        written += (size_t)rc;
    }

    return close(fd);
}

int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
    sigset_t current = 0;
    if (_read_sigmask(&current) < 0) {
        return -1;
    }

    if (oldset) {
        *oldset = current;
    }

    if (!set) {
        return 0;
    }

    const sigset_t blockable_mask =
        ((sigset_t)0x7fffffffU) &
        (sigset_t) ~(1u << (SIGKILL - 1)) &
        (sigset_t) ~(1u << (SIGSTOP - 1));

    sigset_t incoming = *set & blockable_mask;
    sigset_t next = current;

    switch (how) {
    case SIG_BLOCK:
        next |= incoming;
        break;
    case SIG_UNBLOCK:
        next &= (sigset_t)~incoming;
        break;
    case SIG_SETMASK:
        next = incoming;
        break;
    default:
        errno = EINVAL;
        return -1;
    }

    next &= blockable_mask;
    return _write_sigmask(next);
}

int sigpending(sigset_t *set) {
    if (!set) {
        errno = EINVAL;
        return -1;
    }

    proc_stat_t stat = {0};
    if (proc_stat_read_path("/proc/self/stat", &stat) < 0) {
        return -1;
    }

    *set = (sigset_t)stat.signal_pending;
    return 0;
}

int kill(pid_t pid, int signum) {
    long ret = syscall2(SYS_KILL, (uintptr_t)pid, (uintptr_t)signum);

    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }

    return 0;
}

int raise(int signum) {
    return kill(getpid(), signum);
}
