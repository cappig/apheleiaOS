#include "time.h"

#include "stdbool.h"
#include "stdio.h"
#include "string.h"

static const char days_str[7][4] = {
    "Sun",
    "Mon",
    "Tue",
    "Wed",
    "Thu",
    "Fri",
    "Sat",
};

static const char months_str[12][4] = {
    "Jan",
    "Feb",
    "Mar",
    "Apr",
    "May",
    "Jun",
    "Jul",
    "Aug",
    "Sep",
    "Oct",
    "Nov",
    "Dec",
};


static bool _is_leap(int year) {
    return ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0));
}

static unsigned int _days_in_month(int month, int year) {
    switch (month) {
    case 2:
        return _is_leap(year) ? 29 : 28;

    case 1:
    case 3:
    case 5:
    case 7:
    case 8:
    case 10:
    case 12:
        return 31;

    default:
        return 30;
    }
}

static unsigned int _secs_since_epoch(int year) {
    unsigned int days = 0;

    while (year >= 1970) {
        days += 365;

        if (_is_leap(year))
            days += 1;

        year--;
    }

    return days * 24 * 60 * 60;
}

static unsigned int _secs_in_year(int month, int year) {
    unsigned int days = 0;

    for (int i = 0; i < month; i++)
        days += _days_in_month(i + 1, year);

    return days * 24 * 60 * 60;
}


time_t mktime(struct tm* tm) {
    time_t ret = 0;

    ret += _secs_since_epoch(tm->tm_year + 1900 - 1);
    ret += _secs_in_year(tm->tm_mon, tm->tm_year + 1900);
    ret += (tm->tm_mday - 1) * 24 * 60 * 60 + tm->tm_hour * 60 * 60 + tm->tm_min * 60 + tm->tm_sec;

    return ret;
}


// NOTE: this function returns a pointer to static data
char* asctime(const struct tm* time) {
    static char buf[26];

    if (!time)
        return strcpy(buf, "??? ??? ?? ??:??:?? ????");

    sprintf(
        buf,
        "%.3s %.3s%3d %.2d:%.2d:%.2d %d",
        days_str[time->tm_wday],
        months_str[time->tm_mon],
        time->tm_mday,
        time->tm_hour,
        time->tm_min,
        time->tm_sec,
        time->tm_year + 1900
    );

    return buf;
}
