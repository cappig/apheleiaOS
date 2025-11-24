#pragma once

// Error codes as defined by POSIX
// https://pubs.opengroup.org/onlinepubs/9699919799/basedefs/errno.h.html

#define E2BIG           1
#define EACCES          2
#define EADDRINUSE      3
#define EADDRNOTAVAIL   4
#define EAFNOSUPPORT    5
#define EAGAIN          6
#define EWOULDBLOCK     6
#define EALREADY        7
#define EBADF           8
#define EBADMSG         9
#define EBUSY           10
#define ECANCELED       11
#define ECHILD          12
#define ECONNABORTED    13
#define ECONNREFUSED    14
#define ECONNRESET      15
#define EDEADLK         16
#define EDESTADDRREQ    17
#define EDOM            18
#define EDQUOT          19
#define EEXIST          20
#define EFAULT          21
#define EFBIG           22
#define EHOSTUNREACH    23
#define EIDRM           24
#define EILSEQ          25
#define EINPROGRESS     26
#define EINTR           27
#define EINVAL          28
#define EIO             29
#define EISCONN         30
#define EISDIR          31
#define ELOOP           32
#define EMFILE          33
#define EMLINK          34
#define EMSGSIZE        35
#define EMULTIHOP       36
#define ENAMETOOLONG    37
#define ENETDOWN        38
#define ENETRESET       39
#define ENETUNREACH     40
#define ENFILE          41
#define ENOBUFS         42
#define ENODATA         43
#define ENODEV          44
#define ENOENT          45
#define ENOEXEC         46
#define ENOLCK          47
#define ENOLINK         48
#define ENOMEM          49
#define ENOMSG          50
#define ENOPROTOOPT     51
#define ENOSPC          52
#define ENOSR           53
#define ENOSTR          54
#define ENOSYS          55
#define ENOTCONN        56
#define ENOTDIR         57
#define ENOTEMPTY       58
#define ENOTRECOVERABLE 59
#define ENOTSOCK        60
#define ENOTSUP         61
#define EOPNOTSUPP      61
#define ENOTTY          62
#define ENXIO           63
#define EOVERFLOW       64
#define EOWNERDEAD      65
#define EPERM           66
#define EPIPE           67
#define EPROTO          68
#define EPROTONOSUPPORT 69
#define EPROTOTYPE      70
#define ERANGE          71
#define EROFS           72
#define ESPIPE          73
#define ESRCH           74
#define ESTALE          75
#define ETIME           76
#define ETIMEDOUT       77
#define ETXTBSY         78
#define EXDEV           79

#define __SYSCALL_ERRNO(ret)                   \
    ({                                         \
        long _ret = (ret);                     \
        _ret < 0 ? (errno = -_ret, -1) : _ret; \
    })

extern int errno;
