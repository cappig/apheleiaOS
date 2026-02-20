#include "time.h"

#include <stdbool.h>

#include "stdio.h"
#include "string.h"

static const char days_str[7][4] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
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

#define EPOCH_YEAR    1970
#define EPOCH_WDAY    4
#define SECS_PER_MIN  60LL
#define SECS_PER_HOUR (60LL * SECS_PER_MIN)
#define SECS_PER_DAY  (24LL * SECS_PER_HOUR)

static int is_leap(int year) {
    return ((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0);
}

static int days_in_year(int year) {
    return is_leap(year) ? 366 : 365;
}

static int days_in_month(int year, int month) {
    static const int days_norm[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    if (month == 1 && is_leap(year)) {
        return 29;
    }

    return days_norm[month];
}

static bool append_char(char *out, size_t max, size_t *pos, char ch) {
    if (*pos + 1 >= max) {
        return false;
    }

    out[(*pos)++] = ch;
    out[*pos] = '\0';

    return true;
}

static bool append_str(char *out, size_t max, size_t *pos, const char *str) {
    if (!str) {
        return false;
    }

    while (*str) {
        if (!append_char(out, max, pos, *str)) {
            return false;
        }

        str++;
    }

    return true;
}

static bool
append_number(char *out, size_t max, size_t *pos, long long value, int width, char pad) {
    char buf[16];
    int len = snprintf(buf, sizeof(buf), "%lld", value);

    if (len < 0) {
        return false;
    }

    while (len < width) {
        if (!append_char(out, max, pos, pad)) {
            return false;
        }

        width--;
    }

    return append_str(out, max, pos, buf);
}

static const char *weekday_name(int wday) {
    if (wday < 0 || wday > 6) {
        return "???";
    }

    return days_str[wday];
}

static const char *month_name(int month) {
    if (month < 0 || month > 11) {
        return "???";
    }

    return months_str[month];
}

static bool append_hm(char *out, size_t max, size_t *pos, const struct tm *tm) {
    if (!append_number(out, max, pos, tm->tm_hour, 2, '0')) {
        return false;
    }

    if (!append_char(out, max, pos, ':')) {
        return false;
    }

    return append_number(out, max, pos, tm->tm_min, 2, '0');
}

static bool append_hms(char *out, size_t max, size_t *pos, const struct tm *tm) {
    if (!append_hm(out, max, pos, tm)) {
        return false;
    }

    if (!append_char(out, max, pos, ':')) {
        return false;
    }

    return append_number(out, max, pos, tm->tm_sec, 2, '0');
}

static bool append_ymd(char *out, size_t max, size_t *pos, const struct tm *tm) {
    if (!append_number(out, max, pos, tm->tm_year + 1900, 4, '0')) {
        return false;
    }

    if (!append_char(out, max, pos, '-')) {
        return false;
    }

    if (!append_number(out, max, pos, tm->tm_mon + 1, 2, '0')) {
        return false;
    }

    if (!append_char(out, max, pos, '-')) {
        return false;
    }

    return append_number(out, max, pos, tm->tm_mday, 2, '0');
}

static bool append_ctime_layout(char *out, size_t max, size_t *pos, const struct tm *tm) {
    if (!append_str(out, max, pos, weekday_name(tm->tm_wday))) {
        return false;
    }

    if (!append_char(out, max, pos, ' ')) {
        return false;
    }

    if (!append_str(out, max, pos, month_name(tm->tm_mon))) {
        return false;
    }

    if (!append_char(out, max, pos, ' ')) {
        return false;
    }

    if (!append_number(out, max, pos, tm->tm_mday, 2, ' ')) {
        return false;
    }

    if (!append_char(out, max, pos, ' ')) {
        return false;
    }

    if (!append_hms(out, max, pos, tm)) {
        return false;
    }

    if (!append_char(out, max, pos, ' ')) {
        return false;
    }

    return append_number(out, max, pos, tm->tm_year + 1900, 4, '0');
}

static long long days_before_year(int year) {
    long long days = 0;

    if (year >= EPOCH_YEAR) {
        for (int y = EPOCH_YEAR; y < year; y++) {
            days += days_in_year(y);
        }

        return days;
    }

    for (int y = EPOCH_YEAR - 1; y >= year; y--) {
        days -= days_in_year(y);
    }

    return days;
}

time_t mktime(struct tm *tm) {
    if (!tm) {
        return (time_t)-1;
    }

    int year = tm->tm_year + 1900;
    int month = tm->tm_mon;

    while (month < 0) {
        month += 12;
        year--;
    }

    while (month >= 12) {
        month -= 12;
        year++;
    }

    long long days = days_before_year(year);

    for (int m = 0; m < month; m++) {
        days += days_in_month(year, m);
    }

    days += tm->tm_mday - 1;

    long long seconds = days * SECS_PER_DAY;

    seconds += (long long)tm->tm_hour * SECS_PER_HOUR;
    seconds += (long long)tm->tm_min * SECS_PER_MIN;
    seconds += (long long)tm->tm_sec;

    time_t normalized = (time_t)seconds;
    gmtime_r(&normalized, tm);

    return (time_t)seconds;
}

struct tm *gmtime_r(const time_t *timer, struct tm *result) {
    if (!timer || !result) {
        return NULL;
    }

    long long seconds = (long long)*timer;
    long long days = seconds / SECS_PER_DAY;
    long long rem = seconds % SECS_PER_DAY;

    if (rem < 0) {
        rem += SECS_PER_DAY;
        days--;
    }

    result->tm_hour = (int)(rem / SECS_PER_HOUR);
    rem %= SECS_PER_HOUR;
    result->tm_min = (int)(rem / SECS_PER_MIN);
    result->tm_sec = (int)(rem % SECS_PER_MIN);

    int wday = (int)((days + EPOCH_WDAY) % 7);

    if (wday < 0) {
        wday += 7;
    }

    result->tm_wday = wday;

    int year = EPOCH_YEAR;

    while (days >= days_in_year(year)) {
        days -= days_in_year(year);
        year++;
    }

    while (days < 0) {
        year--;
        days += days_in_year(year);
    }

    result->tm_year = year - 1900;
    result->tm_yday = (int)days;

    int month = 0;

    while (month < 11) {
        int dim = days_in_month(year, month);

        if (days < dim) {
            break;
        }

        days -= dim;
        month++;
    }

    result->tm_mon = month;
    result->tm_mday = (int)days + 1;
    result->tm_isdst = 0;

    return result;
}

struct tm *gmtime(const time_t *timer) {
    static struct tm tm_buf;
    return gmtime_r(timer, &tm_buf);
}

struct tm *localtime_r(const time_t *timer, struct tm *result) {
    return gmtime_r(timer, result);
}

struct tm *localtime(const time_t *timer) {
    return gmtime(timer);
}

size_t strftime(char *str, size_t max, const char *format, const struct tm *tm) {
    if (!str || !max || !format || !tm) {
        return 0;
    }

    str[0] = '\0';
    size_t pos = 0;

    while (*format) {
        if (*format != '%') {
            if (!append_char(str, max, &pos, *format)) {
                return 0;
            }

            format++;
            continue;
        }

        format++;
        if (!*format) {
            return 0;
        }

        switch (*format) {
        case '%':
            if (!append_char(str, max, &pos, '%')) {
                return 0;
            }
            break;
        case 'a':
            if (!append_str(str, max, &pos, weekday_name(tm->tm_wday))) {
                return 0;
            }
            break;
        case 'b':
            if (!append_str(str, max, &pos, month_name(tm->tm_mon))) {
                return 0;
            }
            break;
        case 'c':
            if (!append_ctime_layout(str, max, &pos, tm)) {
                return 0;
            }
            break;
        case 'd':
            if (!append_number(str, max, &pos, tm->tm_mday, 2, '0')) {
                return 0;
            }
            break;
        case 'e':
            if (!append_number(str, max, &pos, tm->tm_mday, 2, ' ')) {
                return 0;
            }
            break;
        case 'F':
            if (!append_ymd(str, max, &pos, tm)) {
                return 0;
            }
            break;
        case 'H':
            if (!append_number(str, max, &pos, tm->tm_hour, 2, '0')) {
                return 0;
            }
            break;
        case 'j':
            if (!append_number(str, max, &pos, tm->tm_yday + 1, 3, '0')) {
                return 0;
            }
            break;
        case 'M':
            if (!append_number(str, max, &pos, tm->tm_min, 2, '0')) {
                return 0;
            }
            break;
        case 'm':
            if (!append_number(str, max, &pos, tm->tm_mon + 1, 2, '0')) {
                return 0;
            }
            break;
        case 'R':
            if (!append_hm(str, max, &pos, tm)) {
                return 0;
            }
            break;
        case 'S':
            if (!append_number(str, max, &pos, tm->tm_sec, 2, '0')) {
                return 0;
            }
            break;
        case 's': {
            struct tm copy = *tm;
            if (!append_number(str, max, &pos, (long long)mktime(&copy), 1, '0')) {
                return 0;
            }
            break;
        }
        case 'T':
            if (!append_hms(str, max, &pos, tm)) {
                return 0;
            }
            break;
        case 'u': {
            int iso_wday = tm->tm_wday == 0 ? 7 : tm->tm_wday;
            if (!append_number(str, max, &pos, iso_wday, 1, '0')) {
                return 0;
            }
            break;
        }
        case 'w':
            if (!append_number(str, max, &pos, tm->tm_wday, 1, '0')) {
                return 0;
            }
            break;
        case 'Y':
            if (!append_number(str, max, &pos, tm->tm_year + 1900, 4, '0')) {
                return 0;
            }
            break;
        case 'y':
            if (!append_number(str, max, &pos, (tm->tm_year + 1900) % 100, 2, '0')) {
                return 0;
            }
            break;
        default:
            if (!append_char(str, max, &pos, '%')) {
                return 0;
            }
            if (!append_char(str, max, &pos, *format)) {
                return 0;
            }
            break;
        }

        format++;
    }

    return pos;
}

char *asctime(const struct tm *time) {
    static char buf[32];

    if (!time) {
        return strcpy(buf, "??? ??? ?? ??:??:?? ????\n");
    }

    if (!strftime(buf, sizeof(buf), "%a %b %e %H:%M:%S %Y\n", time)) {
        return strcpy(buf, "??? ??? ?? ??:??:?? ????\n");
    }

    return buf;
}

char *ctime(const time_t *timer) {
    struct tm tm_buf;

    if (!localtime_r(timer, &tm_buf)) {
        return asctime(NULL);
    }

    return asctime(&tm_buf);
}
