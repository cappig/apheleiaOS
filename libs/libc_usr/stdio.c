#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PRINTF_STACK_BUF 1024

#define FILE_FLAG_READ   (1 << 0)
#define FILE_FLAG_WRITE  (1 << 1)
#define FILE_FLAG_APPEND (1 << 2)
#define FILE_FLAG_STATIC (1 << 3)

static FILE std_in = {
    .fd = STDIN_FILENO,
    .flags = FILE_FLAG_READ | FILE_FLAG_STATIC,
    .eof = 0,
    .error = 0,
};

static FILE std_out = {
    .fd = STDOUT_FILENO,
    .flags = FILE_FLAG_WRITE | FILE_FLAG_STATIC,
    .eof = 0,
    .error = 0,
};

static FILE std_err = {
    .fd = STDERR_FILENO,
    .flags = FILE_FLAG_WRITE | FILE_FLAG_STATIC,
    .eof = 0,
    .error = 0,
};

FILE *stdin = &std_in;
FILE *stdout = &std_out;
FILE *stderr = &std_err;


static int write_all_fd(int fd, const char *buf, size_t len) {
    size_t off = 0;

    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n <= 0) {
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

static int stream_write_all(FILE *stream, const char *buf, size_t len) {
    if (!stream || stream->fd < 0 || !(stream->flags & FILE_FLAG_WRITE)) {
        errno = EBADF;
        if (stream) {
            stream->error = 1;
        }
        return -1;
    }

    if (write_all_fd(stream->fd, buf, len) < 0) {
        stream->error = 1;
        return -1;
    }

    return 0;
}

static int mode_to_flags(const char *mode, int *open_flags, int *stream_flags) {
    if (!mode || !mode[0] || !open_flags || !stream_flags) {
        return -1;
    }

    bool plus = false;
    for (const char *p = mode + 1; *p; p++) {
        if (*p == '+') {
            plus = true;
        } else if (*p != 'b') {
            return -1;
        }
    }

    switch (mode[0]) {
    case 'r':
        *open_flags = plus ? O_RDWR : O_RDONLY;
        *stream_flags = FILE_FLAG_READ | (plus ? FILE_FLAG_WRITE : 0);
        return 0;
    case 'w':
        *open_flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC;
        *stream_flags = FILE_FLAG_WRITE | (plus ? FILE_FLAG_READ : 0);
        return 0;
    case 'a':
        *open_flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND;
        *stream_flags =
            FILE_FLAG_APPEND | FILE_FLAG_WRITE | (plus ? FILE_FLAG_READ : 0);
        return 0;
    default:
        return -1;
    }
}

FILE *fdopen(int fd, const char *mode) {
    if (fd < 0) {
        errno = EBADF;
        return NULL;
    }

    int open_flags = 0;
    int stream_flags = 0;
    if (mode_to_flags(mode, &open_flags, &stream_flags) < 0) {
        errno = EINVAL;
        return NULL;
    }

    if (stream_flags & FILE_FLAG_APPEND) {
        (void)lseek(fd, 0, SEEK_END);
    }

    FILE *stream = malloc(sizeof(*stream));
    if (!stream) {
        errno = ENOMEM;
        return NULL;
    }

    stream->fd = fd;
    stream->flags = stream_flags;
    stream->eof = 0;
    stream->error = 0;
    return stream;
}

FILE *fopen(const char *path, const char *mode) {
    int open_flags = 0;
    int stream_flags = 0;
    if (mode_to_flags(mode, &open_flags, &stream_flags) < 0) {
        errno = EINVAL;
        return NULL;
    }

    int fd = 
        (open_flags & O_CREAT) ? open(path, open_flags, 0666) : open(path, open_flags);

    if (fd < 0) {
        return NULL;
    }

    FILE *stream = malloc(sizeof(*stream));
    if (!stream) {
        close(fd);
        errno = ENOMEM;
        return NULL;
    }

    stream->fd = fd;
    stream->flags = stream_flags;
    stream->eof = 0;
    stream->error = 0;
    return stream;
}

int remove(const char *path) {
    if (!path) {
        errno = EINVAL;
        return -1;
    }

    if (!unlink(path)) {
        return 0;
    }

    if (errno == EISDIR || errno == EPERM || errno == EACCES) {
        return rmdir(path);
    }

    return -1;
}

int fflush(FILE *stream) {
    if (!stream) {
        return 0;
    }

    if (stream->fd < 0) {
        errno = EBADF;
        stream->error = 1;
        return EOF;
    }

    return 0;
}

int fclose(FILE *stream) {
    if (!stream || stream->fd < 0) {
        errno = EBADF;
        return EOF;
    }

    (void)fflush(stream);
    int ret = close(stream->fd);
    stream->fd = -1;

    if (!(stream->flags & FILE_FLAG_STATIC)) {
        free(stream);
    }

    return ret < 0 ? EOF : 0;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (!ptr || !stream || !size || !nmemb) {
        return 0;
    }

    if (stream->fd < 0 || !(stream->flags & FILE_FLAG_READ)) {
        errno = EBADF;
        stream->error = 1;
        return 0;
    }

    size_t want = size * nmemb;
    if (size && want / size != nmemb) {
        errno = EOVERFLOW;
        stream->error = 1;
        return 0;
    }

    size_t got = 0;
    while (got < want) {
        ssize_t n = read(stream->fd, (char *)ptr + got, want - got);
        if (n < 0) {
            stream->error = 1;
            break;
        }
        if (!n) {
            stream->eof = 1;
            break;
        }
        got += (size_t)n;
    }

    return got / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (!ptr || !stream || !size || !nmemb) {
        return 0;
    }

    if (stream->fd < 0 || !(stream->flags & FILE_FLAG_WRITE)) {
        errno = EBADF;
        stream->error = 1;
        return 0;
    }

    size_t want = size * nmemb;
    if (size && want / size != nmemb) {
        errno = EOVERFLOW;
        stream->error = 1;
        return 0;
    }

    size_t sent = 0;
    while (sent < want) {
        ssize_t n = write(stream->fd, (const char *)ptr + sent, want - sent);
        if (n <= 0) {
            stream->error = 1;
            break;
        }
        sent += (size_t)n;
    }

    return sent / size;
}

int fseek(FILE *stream, long offset, int whence) {
    if (!stream || stream->fd < 0) {
        errno = EBADF;
        if (stream) {
            stream->error = 1;
        }
        return -1;
    }

    if (lseek(stream->fd, (off_t)offset, whence) < 0) {
        stream->error = 1;
        return -1;
    }

    stream->eof = 0;
    return 0;
}

long ftell(FILE *stream) {
    if (!stream || stream->fd < 0) {
        errno = EBADF;
        if (stream) {
            stream->error = 1;
        }
        return -1;
    }

    off_t off = lseek(stream->fd, 0, SEEK_CUR);
    if (off < 0) {
        stream->error = 1;
        return -1;
    }

    return (long)off;
}

int fgetc(FILE *stream) {
    unsigned char ch = 0;
    size_t n = fread(&ch, 1, 1, stream);
    if (n != 1) {
        return EOF;
    }
    return (int)ch;
}

int fputc(int ch, FILE *stream) {
    unsigned char c = (unsigned char)ch;
    size_t n = fwrite(&c, 1, 1, stream);
    return n == 1 ? (int)c : EOF;
}

char *fgets(char *str, int n, FILE *stream) {
    if (!str || n <= 0 || !stream) {
        errno = EINVAL;
        if (stream) {
            stream->error = 1;
        }
        return NULL;
    }

    int i = 0;
    while (i < n - 1) {
        int ch = fgetc(stream);
        if (ch == EOF) {
            break;
        }

        str[i++] = (char)ch;
        if (ch == '\n') {
            break;
        }
    }

    if (!i) {
        return NULL;
    }

    str[i] = '\0';
    return str;
}

int fputs(const char *str, FILE *stream) {
    if (!str) {
        errno = EINVAL;
        if (stream) {
            stream->error = 1;
        }
        return EOF;
    }

    size_t len = strlen(str);
    if (stream_write_all(stream, str, len) < 0) {
        return EOF;
    }

    return (int)len;
}

int feof(FILE *stream) {
    return stream ? stream->eof : 0;
}

int ferror(FILE *stream) {
    return stream ? stream->error : 0;
}

void clearerr(FILE *stream) {
    if (!stream) {
        return;
    }
    stream->eof = 0;
    stream->error = 0;
}

int setvbuf(FILE *restrict stream, char *restrict buf, int mode, size_t size) {
    (void)buf;
    (void)size;

    if (!stream) {
        errno = EINVAL;
        return -1;
    }

    if (mode != _IOFBF && mode != _IOLBF && mode != _IONBF) {
        errno = EINVAL;
        return -1;
    }

    return 0;
}

void setbuf(FILE *restrict stream, char *restrict buf) {
    (void)setvbuf(stream, buf, buf ? _IOFBF : _IONBF, BUFSIZ);
}

int fileno(FILE *stream) {
    if (!stream || stream->fd < 0) {
        errno = EBADF;
        return -1;
    }

    return stream->fd;
}

int ungetc(int ch, FILE *stream) {
    if (ch == EOF) {
        return EOF;
    }

    if (!stream || stream->fd < 0 || !(stream->flags & FILE_FLAG_READ)) {
        errno = EBADF;
        if (stream) {
            stream->error = 1;
        }
        return EOF;
    }

    off_t current = lseek(stream->fd, 0, SEEK_CUR);
    if (current <= 0) {
        errno = ENOTSUP;
        stream->error = 1;
        return EOF;
    }

    if (lseek(stream->fd, -1, SEEK_CUR) < 0) {
        stream->error = 1;
        return EOF;
    }

    stream->eof = 0;
    return (unsigned char)ch;
}

int vfprintf(FILE *stream, const char *restrict format, va_list vlist) {
    if (!stream || !format) {
        errno = EINVAL;
        if (stream) {
            stream->error = 1;
        }
        return -1;
    }

    char stack_buf[PRINTF_STACK_BUF];
    va_list args_copy;
    va_copy(args_copy, vlist);
    int needed = vsnprintf(stack_buf, sizeof(stack_buf), format, args_copy);
    va_end(args_copy);

    if (needed < 0) {
        stream->error = 1;
        return -1;
    }

    size_t bytes = (size_t)needed;
    char *buf = stack_buf;

    if (bytes >= sizeof(stack_buf)) {
        buf = malloc(bytes + 1);
        if (!buf) {
            errno = ENOMEM;
            stream->error = 1;
            return -1;
        }

        va_copy(args_copy, vlist);
        int rebuilt = vsnprintf(buf, bytes + 1, format, args_copy);
        va_end(args_copy);
        if (rebuilt < 0) {
            free(buf);
            stream->error = 1;
            return -1;
        }
    }

    int ret = needed;
    if (stream_write_all(stream, buf, bytes) < 0) {
        ret = -1;
    }

    if (buf != stack_buf) {
        free(buf);
    }

    return ret;
}

int fprintf(FILE *restrict stream, const char *restrict format, ...) {
    va_list args;
    va_start(args, format);
    int ret = vfprintf(stream, format, args);
    va_end(args);
    return ret;
}

int printf(const char *restrict format, ...) {
    va_list args;
    va_start(args, format);
    int ret = vfprintf(stdout, format, args);
    va_end(args);
    return ret;
}

int getchar(void) {
    return fgetc(stdin);
}

int putchar(int ch) {
    return fputc(ch, stdout);
}

int puts(const char *str) {
    if (fputs(str, stdout) == EOF) {
        return EOF;
    }
    return fputc('\n', stdout);
}

void perror(const char *s) {
    int saved = errno;
    if (s && s[0]) {
        (void)fputs(s, stderr);
        (void)fputs(": ", stderr);
    }
    (void)fputs(strerror(saved), stderr);
    (void)fputc('\n', stderr);
}
