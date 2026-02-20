#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <sys/types.h>

#define EOF      (-1)
#define BUFSIZ   1024
#define FILENAME_MAX 255
#define L_tmpnam 20
#define TMP_MAX  10000

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

typedef struct FILE {
    int fd;
    int flags;
    int eof;
    int error;
} FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

int fflush(FILE *stream);
int fclose(FILE *stream);
FILE *fopen(const char *path, const char *mode);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
int fseek(FILE *stream, long offset, int whence);
long ftell(FILE *stream);
int fgetc(FILE *stream);
int fputc(int ch, FILE *stream);
char *fgets(char *str, int n, FILE *stream);
int fputs(const char *str, FILE *stream);
int feof(FILE *stream);
int ferror(FILE *stream);
void clearerr(FILE *stream);

int vfprintf(FILE *stream, const char *restrict format, va_list vlist);
int fprintf(FILE *restrict stream, const char *restrict format, ...)
    __attribute__((format(printf, 2, 3)));

int vsnprintf(char *restrict buffer, size_t max_size, const char *restrict format, va_list vlist);
int vsprintf(char *restrict buffer, const char *restrict format, va_list vlist);

int snprintf(char *restrict buffer, size_t max_size, const char *restrict format, ...)
    __attribute__((format(printf, 3, 4)));
int sprintf(char *restrict buffer, const char *restrict format, ...)
    __attribute__((format(printf, 2, 3)));
int printf(const char *restrict format, ...) __attribute__((format(printf, 1, 2)));

int getchar(void);
int putchar(int ch);
int puts(const char *str);
void perror(const char *s);

int vsscanf(const char *restrict str, const char *restrict format, va_list vlist);

#ifdef _APHELEIA_SOURCE
int vsnscanf(const char *restrict str, size_t max, const char *restrict format, va_list vlist);
int snscanf(const char *restrict str, size_t max, const char *restrict format, ...)
    __attribute__((format(scanf, 3, 4)));
#endif
int sscanf(const char *restrict str, const char *restrict format, ...)
    __attribute__((format(scanf, 2, 3)));
