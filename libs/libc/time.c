#include "time.h"

#include <limits.h>
#include <stdbool.h>

#include "stdio.h"
#include "string.h"

static const char days_str[7][4] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };

static const char months_str[12][4] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
};

#define EPOCH_YEAR    1970
#define EPOCH_WDAY    4
#define SECS_PER_MIN  60LL
#define SECS_PER_HOUR (60LL * SECS_PER_MIN)
#define SECS_PER_DAY  (24LL * SECS_PER_HOUR)


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

static bool append_number(char *out, size_t max, size_t *pos, long long value, int width, char pad) {
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

static long long days_from_civil(long long year, int month, int day) {
    year -= month <= 2;

    long long era = year >= 0 ? year / 400 : (year - 399) / 400;
    unsigned yoe = (unsigned)(year - era * 400);
    unsigned m = (unsigned)(month + (month > 2 ? -3 : 9));
    unsigned doy = (153U * m + 2U) / 5U + (unsigned)day - 1U;
    unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;

    return era * 146097LL + (long long)doe - 719468LL;
}

static bool civil_from_days(long long days, long long *year_out, int *month_out, int *day_out, int *yday_out) {
    if (!year_out || !month_out || !day_out || !yday_out) {
        return false;
    }

    long long z = days + 719468LL;
    long long era = z >= 0 ? z / 146097LL : (z - 146096LL) / 146097LL;
    unsigned doe = (unsigned)(z - era * 146097LL);
    unsigned yoe = (doe - doe / 1460U + doe / 36524U - doe / 146096U) / 365U;
    long long year = (long long)yoe + era * 400LL;
    unsigned yday = doe - (365U * yoe + yoe / 4U - yoe / 100U);
    unsigned mp = (5U * yday + 2U) / 153U;
    unsigned day = yday - (153U * mp + 2U) / 5U + 1U;
    int month = (int)mp + (mp < 10U ? 3 : -9);

    year += month <= 2;

    *year_out = year;
    *month_out = month;
    *day_out = (int)day;
    *yday_out = (int)(days - days_from_civil(year, 1, 1));
    return true;
}

time_t mktime(struct tm *tm) {
    if (!tm) {
        return (time_t)-1;
    }

    long long year = (long long)tm->tm_year + 1900LL;
    long long month = tm->tm_mon;
    long long month_years = month / 12LL;
    int month_in_year = (int)(month % 12LL);

    if (month_in_year < 0) {
        month_in_year += 12;
        month_years--;
    }

    year += month_years;

    long long days = days_from_civil(year, month_in_year + 1, tm->tm_mday);
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

    long long year = 0;
    int month = 0;
    int mday = 0;
    int yday = 0;

    if (!civil_from_days(days, &year, &month, &mday, &yday)) {
        return NULL;
    }

    bool year_too_small = year < (long long)INT_MIN + 1900LL;
    bool year_too_large = year > (long long)INT_MAX + 1900LL;

    if (year_too_small || year_too_large) {
        return NULL;
    }

    result->tm_year = (int)(year - 1900LL);
    result->tm_yday = yday;
    result->tm_mon = month - 1;
    result->tm_mday = mday;
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

static char asctime_buf[32];

static char *asctime_bad_time(void) {
    static const char text[] = "??? ??? ?? ??:??:?? ????\n";
    memcpy(asctime_buf, text, sizeof(text));
    return asctime_buf;
}

char *asctime(const struct tm *time) {
    if (!time) {
        return asctime_bad_time();
    }

    size_t len = strftime(asctime_buf, sizeof(asctime_buf), "%a %b %e %H:%M:%S %Y\n", time);

    if (!len) {
        return asctime_bad_time();
    }

    return asctime_buf;
}

char *ctime(const time_t *timer) {
    struct tm tm_buf;

    if (!timer || !localtime_r(timer, &tm_buf)) {
        return asctime_bad_time();
    }

    return asctime(&tm_buf);
}
