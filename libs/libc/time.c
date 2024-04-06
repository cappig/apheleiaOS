#include "time.h"

#include "stdio.h"
#include "string.h"


// TODO: this is dogshit
char* asctime(const struct tm* time) {
    static const char days[7][4] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

    static const char months[12][4] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };

    static char buf[26];

    if (!time)
        return strcpy(buf, "??? ??? ?? ??:??:?? ????\n");

    snprintf(
        buf,
        26,
        "%.3s %.3s%3d %.2d:%.2d:%.2d %d\n",
        days[time->tm_wday],
        months[time->tm_mon],
        time->tm_mday,
        time->tm_hour,
        time->tm_min,
        time->tm_sec,
        1900 + time->tm_year
    );

    return buf;
}
