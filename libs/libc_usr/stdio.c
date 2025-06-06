#include "stdio.h"

#include <aos/syscalls.h>
#include <stddef.h>
#include <string.h>

#include "stdlib.h"


static int _read_byte(FILE* stream) {
    if (stream->flags & FLAG_ERROR)
        return EOF;

    if (stream->flags & FLAG_EOF)
        return EOF;

    if (!(stream->mode & MODE_READ)) {
        stream->flags |= FLAG_ERROR;
        return EOF;
    }

    // the buffer is empty, read more data
    if (stream->pos >= stream->len) {
        // if in write mode, flush the buffer first
        if (stream->mode & MODE_WRITE)
            if (fflush(stream))
                return EOF;

        ssize_t read = sys_read(stream->fd, stream->buf, stream->buf_size);

        if (read < 0) {
            stream->flags |= FLAG_ERROR;
            return EOF;
        }

        if (!read) {
            stream->flags |= FLAG_EOF;
            return EOF;
        }

        stream->len = read;
        stream->pos = 0;
    }

    return stream->buf[stream->pos++];
}

static int _write_byte(char c, FILE* stream) {
    if (stream->flags & FLAG_ERROR)
        return EOF;

    if (!(stream->mode & MODE_WRITE)) {
        stream->flags |= FLAG_ERROR;
        return EOF;
    }

    // if in read mode with write permissions flush the buffer and seek to current file position
    if ((stream->mode & MODE_READ) && (stream->mode & MODE_PLUS)) {
        if (fflush(stream))
            return EOF;

        long pos = ftell(stream);

        if (pos == EOF)
            return EOF;

        if (fseek(stream, pos, SEEK_SET))
            return EOF;
    }

    // buffer is full, flush
    if (stream->pos >= stream->buf_size)
        if (fflush(stream))
            return EOF;

    stream->buf[stream->pos++] = c;

    // line buffering - flush on newline
    if (stream->buf_mode == _IOLBF && c == '\n')
        if (fflush(stream))
            return EOF;

    return c;
}


static int _parse_mode(const char* mode_str) {
    int mode = 0;

    switch (*mode_str) {
    case 'r':
        mode |= MODE_READ;
        break;

    case 'w':
        mode |= MODE_WRITE;
        break;

    case 'a':
        mode |= MODE_WRITE | MODE_APPEND;
        break;

    default:
        return -1;
    }

    mode_str++;

    while (*mode_str) {
        switch (*mode_str) {
        case '+':
            mode |= MODE_PLUS;
            break;

        case 'b':
            break;

        default:
            return -1;
        }
        mode_str++;
    }

    return mode;
}

static int _mode_to_open_flags(int mode) {
    int flags = 0;

    if (mode & MODE_PLUS)
        flags = O_RDWR;
    else if (mode & MODE_WRITE)
        flags = O_WRONLY;
    else
        flags = O_RDONLY;

    if (mode & MODE_WRITE) {
        flags |= O_CREAT;

        if (mode & MODE_APPEND)
            flags |= O_APPEND;
        else
            flags |= O_TRUNC;
    }

    return flags;
}


FILE* fopen(const char* restrict pathname, const char* restrict mode_str) {
    FILE* file = calloc(1, sizeof(FILE));

    if (!file)
        return NULL;

    file->mode = _parse_mode(mode_str);

    if (file->mode < 0) {
        free(file);
        return NULL;
    }

    int open_flags = _mode_to_open_flags(file->mode);
    file->fd = sys_open(pathname, open_flags, 0666);

    if (file->fd < 0) {
        free(file);
        return NULL;
    }

    file->buf = calloc(1, BUFSIZ);

    if (!file->buf) {
        sys_close(file->fd);
        free(file);
        return NULL;
    }

    file->buf_size = BUFSIZ;
    file->buf_mode = _IOFBF;

    return file;
}

int fclose(FILE* file) {
    if (!file)
        return EOF;

    fflush(file);

    int close = sys_close(file->fd);

    if (file->buf)
        free(file->buf);

    free(file);

    return (close < 0) ? EOF : 0;
}

FILE* freopen(const char* restrict pathname, const char* restrict mode, FILE* restrict file) {
    if (!file)
        return NULL;

    if (fclose(file))
        return NULL;

    return fopen(pathname, mode);
}


int fflush(FILE* file) {
    // TODO: fflush(NULL) should flush all streams
    if (!file)
        return EOF;

    if (file->mode & MODE_WRITE) {
        if (file->pos > 0) {
            ssize_t write = sys_write(file->fd, file->buf, file->pos);

            // Check if all data was written
            if (write < 0 || (size_t)write != file->pos) {
                file->flags |= FLAG_ERROR;
                return EOF;
            }

            file->pos = 0;
        }
    } else {
        // For read only streams any buffered data is discarded, as per POSIX
        file->pos = 0;
        file->len = 0;
    }

    return 0;
}


int setvbuf(FILE* restrict file, char* restrict buf, int mode, size_t size) {
    if (!file)
        return EOF;

    if (mode != _IOFBF && mode != _IOLBF && mode != _IONBF)
        return EOF;

    if (fflush(file))
        return EOF;

    if (file->buf && !(file->flags & FLAG_USER_BUF))
        free(file->buf);

    file->flags &= ~FLAG_USER_BUF;
    file->buf = NULL;
    file->buf_size = 0;

    file->buf_mode = mode;
    file->pos = 0;
    file->len = 0;

    if (mode != _IONBF) {
        // user provided buffer
        if (buf) {
            file->buf = (unsigned char*)buf;
            file->flags |= FLAG_USER_BUF;
        } else {
            file->buf = calloc(1, size);

            if (!file->buf) {
                file->buf_mode = _IONBF;
                file->buf_size = 0;
                return EOF;
            }
        }

        file->buf_size = size;
    }

    return 0;
}

void setbuf(FILE* restrict file, char* restrict buf) {
    setvbuf(file, buf, buf ? _IOFBF : _IONBF, BUFSIZ);
}


int fgetc(FILE* stream) {
    if (!stream)
        return EOF;

    return _read_byte(stream);
}

int fputc(int c, FILE* stream) {
    if (!stream)
        return EOF;

    return _write_byte(c, stream);
}

size_t fread(void* restrict ptr, size_t size, size_t nmemb, FILE* restrict file) {
    if (!file || !ptr || !size || !nmemb)
        return 0;

    size_t total = size * nmemb;
    size_t read = 0;
    unsigned char* dest = (unsigned char*)ptr;

    while (read < total) {
        int c = _read_byte(file);

        // EOF or error
        if (c < 0)
            break;

        dest[read++] = (unsigned char)c;
    }

    return read / size;
}

size_t fwrite(const void* restrict ptr, size_t size, size_t nmemb, FILE* restrict file) {
    if (!file || !ptr || !size || !nmemb)
        return 0;

    if (!(file->mode & MODE_WRITE)) {
        file->flags |= FLAG_ERROR;
        return 0;
    }

    size_t total = size * nmemb;
    size_t written = 0;
    const unsigned char* src = (const unsigned char*)ptr;

    while (written < total) {
        int result = _write_byte(src[written], file);

        // error
        if (result < 0)
            break;

        written++;
    }

    return written / size;
}

char* fgets(char* restrict s, int n, FILE* restrict stream) {
    if (!stream || !s || n <= 0) {
        return NULL;
    }
    if (!(stream->mode & MODE_READ)) {
        stream->flags |= FLAG_ERROR;
        return NULL;
    }

    int pos = 0;
    int ch;

    while (pos < n - 1) {
        ch = _read_byte(stream);

        if (ch == EOF) {
            // nothing to read
            if (!pos)
                return NULL;

            break;
        }

        s[pos++] = (char)ch;

        if (ch == '\n')
            break;
    }

    s[pos] = '\0';

    return s;
}

int fputs(const char* restrict s, FILE* restrict stream) {
    if (!stream || !s)
        return EOF;

    if (!(stream->mode & MODE_WRITE)) {
        stream->flags |= FLAG_ERROR;
        return EOF;
    }

    size_t written = 0;

    while (*s) {
        int write = _write_byte(*s, stream);

        if (write == EOF)
            return EOF;

        written++;
        s++;
    }

    return written;
}


int fgetpos(FILE* restrict f, fpos_t* restrict pos) {
    if (!f || !pos)
        return EOF;

    off_t seek = sys_seek(f->fd, 0, SEEK_CUR);

    if (seek < 0) {
        f->flags |= FLAG_ERROR;
        return EOF;
    }

    if (f->mode & MODE_READ)
        *pos = seek - (f->len - f->pos);
    else
        *pos = seek + f->pos;

    return 0;
}


int fsetpos(FILE* f, const fpos_t* pos) {
    if (!f || !pos)
        return EOF;

    if (fflush(f))
        return EOF;

    off_t seek = sys_seek(f->fd, *pos, SEEK_SET);

    if (seek < 0) {
        f->flags |= FLAG_ERROR;
        return EOF;
    }

    f->pos = 0;
    f->len = 0;
    f->flags &= ~(FLAG_EOF);

    return 0;
}

int fseek(FILE* f, long offset, int whence) {
    if (!f)
        return EOF;

    if (fflush(f))
        return EOF;

    off_t seek = sys_seek(f->fd, offset, whence);

    if (seek < 0) {
        f->flags |= FLAG_ERROR;
        return EOF;
    }

    f->pos = 0;
    f->len = 0;
    f->flags &= ~(FLAG_EOF);

    return 0;
}

void rewind(FILE* f) { // where did the f go ??
    if (!f)
        return;

    fseek(f, 0, SEEK_SET);
    f->flags &= ~(FLAG_ERROR | FLAG_EOF);
}


long ftell(FILE* f) {
    if (!f)
        return EOF;

    // get the current offset
    off_t seek = sys_seek(f->fd, 0, SEEK_CUR);

    if (seek < 0) {
        f->flags |= FLAG_ERROR;
        return EOF;
    }

    if (f->mode & MODE_READ)
        return seek - (f->len - f->pos);
    else
        return seek + f->pos;
}


int feof(FILE* f) {
    if (!f)
        return EOF;

    return f->flags & FLAG_EOF;
}

int ferror(FILE* f) {
    if (!f)
        return EOF;

    return f->flags & FLAG_ERROR;
}

void clearerr(FILE* f) {
    if (!f)
        return;

    f->flags &= ~(FLAG_EOF | FLAG_ERROR);
}
