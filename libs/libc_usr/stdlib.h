#pragma once

#include <stddef.h>

char *getenv(const char *name);
int putenv(char *string);
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
int clearenv(void);
char *realpath(const char *path, char *resolved_path);
int mkstemp(char *template);
int mkstemps(char *template, int suffix_len);

int atexit(void (*fn)(void));
void exit(int status) __attribute__((noreturn));
void _Exit(int status) __attribute__((noreturn));

int system(const char *command);
