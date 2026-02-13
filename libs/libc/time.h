#pragma once

#include <stddef.h>

typedef long int time_t;

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

time_t time(time_t* timer);

time_t mktime(struct tm* tm);

struct tm* gmtime_r(const time_t* timer, struct tm* result);
struct tm* gmtime(const time_t* timer);
struct tm* localtime_r(const time_t* timer, struct tm* result);
struct tm* localtime(const time_t* timer);

size_t strftime(char* str, size_t max, const char* format, const struct tm* tm);

char* asctime(const struct tm* time);
char* ctime(const time_t* timer);
