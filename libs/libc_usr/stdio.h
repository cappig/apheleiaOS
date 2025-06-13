#pragma once

#include "stdarg.h"
#include "stdlib.h"

#define EOF (-1)

#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

#define BUFSIZ 8192

#define MODE_READ   (1 << 0)
#define MODE_WRITE  (1 << 1)
#define MODE_APPEND (1 << 2)
#define MODE_PLUS   (1 << 3)

#define FLAG_EOF      (1 << 0)
#define FLAG_ERROR    (1 << 1)
#define FLAG_USER_BUF (1 << 2)

#define SEEK_CUR SYS_SEEK_CUR
#define SEEK_END SYS_SEEK_END
#define SEEK_SET SYS_SEEK_SET

typedef ptrdiff_t ssize_t;
typedef ssize_t fpos_t;

typedef struct {
    int fd;

    unsigned char* buf;
    size_t buf_size;

    size_t pos;
    size_t len;

    char buf_mode;
    char mode;
    char flags;
} FILE;

extern FILE* stdin;
extern FILE* stdout;
extern FILE* stderr;


FILE* fopen(const char* restrict pathname, const char* restrict mode_str);
FILE* freopen(const char* restrict pathname, const char* restrict mode, FILE* restrict f);
int fclose(FILE* file);

int fflush(FILE* file);

int setvbuf(FILE* restrict file, char* restrict buf, int mode, size_t size);
void setbuf(FILE* restrict file, char* restrict buf);

size_t fread(void* restrict ptr, size_t size, size_t nmemb, FILE* restrict file);
size_t fwrite(const void* restrict ptr, size_t size, size_t nmemb, FILE* restrict file);

char* fgets(char* restrict s, int n, FILE* restrict stream);
int fputs(const char* restrict s, FILE* restrict stream);
int fgetc(FILE* stream);
int fputc(int c, FILE* stream);

int fgetpos(FILE* restrict f, fpos_t* restrict pos);
int fsetpos(FILE* f, const fpos_t* pos);

int fseek(FILE* f, long offset, int whence);
void rewind(FILE* f);

long ftell(FILE* f);

int feof(FILE* f);
int ferror(FILE* f);
void clearerr(FILE* f);

int vfprintf(FILE* restrict stream, const char* restrict format, va_list vlist);

int fprintf(FILE* restrict stream, const char* restrict format, ...)
    __attribute__((format(printf, 2, 3)));

int printf(const char* restrict format, ...) __attribute__((format(printf, 1, 2)));
