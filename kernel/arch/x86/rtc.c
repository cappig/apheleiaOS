#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <x86/asm.h>
#include <x86/rtc.h>

#define CMOS_ADDR_PORT    0x70
#define CMOS_DATA_PORT    0x71
#define CMOS_NMI_DISABLE  0x80

#define RTC_REG_SECONDS   0x00
#define RTC_REG_MINUTES   0x02
#define RTC_REG_HOURS     0x04
#define RTC_REG_WEEKDAY   0x06
#define RTC_REG_DAY       0x07
#define RTC_REG_MONTH     0x08
#define RTC_REG_YEAR      0x09
#define RTC_REG_STATUS_A  0x0A
#define RTC_REG_STATUS_B  0x0B
#define RTC_REG_CENTURY   0x32

#define RTC_A_UIP         0x80
#define RTC_B_BINARY      0x04
#define RTC_B_HOUR_24     0x02

typedef struct {
    u8 sec;
    u8 min;
    u8 hour;
    u8 wday;
    u8 mday;
    u8 mon;
    u8 year;
    u8 century;
    u8 status_b;
} rtc_sample_t;

static u8 _cmos_read(u8 reg) {
    outb(CMOS_ADDR_PORT, (u8)(CMOS_NMI_DISABLE | reg));
    return inb(CMOS_DATA_PORT);
}

static bool _update_in_progress(void) {
    return _cmos_read(RTC_REG_STATUS_A) & RTC_A_UIP;
}

static u8 _bcd_to_bin(u8 value) {
    return (u8)(((value >> 4) * 10) + (value & 0x0F));
}

static void _read_sample(rtc_sample_t* sample) {
    if (!sample)
        return;

    sample->sec = _cmos_read(RTC_REG_SECONDS);
    sample->min = _cmos_read(RTC_REG_MINUTES);
    sample->hour = _cmos_read(RTC_REG_HOURS);
    sample->wday = _cmos_read(RTC_REG_WEEKDAY);
    sample->mday = _cmos_read(RTC_REG_DAY);
    sample->mon = _cmos_read(RTC_REG_MONTH);
    sample->year = _cmos_read(RTC_REG_YEAR);
    sample->century = _cmos_read(RTC_REG_CENTURY);
    sample->status_b = _cmos_read(RTC_REG_STATUS_B);
}

static void _normalize_sample(rtc_sample_t* sample) {
    if (!sample)
        return;

    bool binary_mode = sample->status_b & RTC_B_BINARY;
    bool hour_24 = sample->status_b & RTC_B_HOUR_24;

    if (!binary_mode) {
        sample->sec = _bcd_to_bin(sample->sec);
        sample->min = _bcd_to_bin(sample->min);

        u8 hour = sample->hour;
        u8 pm = (u8)(hour & 0x80);
        hour = _bcd_to_bin((u8)(hour & 0x7F));
        sample->hour = (u8)(hour | pm);

        sample->wday = _bcd_to_bin(sample->wday);
        sample->mday = _bcd_to_bin(sample->mday);
        sample->mon = _bcd_to_bin(sample->mon);
        sample->year = _bcd_to_bin(sample->year);
        sample->century = _bcd_to_bin(sample->century);
    }

    if (!hour_24) {
        bool is_pm = sample->hour & 0x80;
        sample->hour &= 0x7F;

        if (is_pm && sample->hour < 12)
            sample->hour = (u8)(sample->hour + 12);
        if (!is_pm && sample->hour == 12)
            sample->hour = 0;
    }
}

static int _full_year(const rtc_sample_t* sample) {
    if (!sample)
        return 0;

    if (sample->century)
        return (int)sample->century * 100 + (int)sample->year;

    if (sample->year >= 70)
        return 1900 + sample->year;

    return 2000 + sample->year;
}

static bool _sample_valid(const rtc_sample_t* sample) {
    if (!sample)
        return false;

    if (sample->sec > 59 || sample->min > 59 || sample->hour > 23)
        return false;

    if (!sample->mday || sample->mday > 31)
        return false;

    if (!sample->mon || sample->mon > 12)
        return false;

    return true;
}

u64 x86_rtc_unix_seconds(void) {
    rtc_sample_t first = {0};
    rtc_sample_t second = {0};

    while (_update_in_progress())
        cpu_pause();

    _read_sample(&first);

    while (_update_in_progress())
        cpu_pause();

    _read_sample(&second);

    if (memcmp(&first, &second, sizeof(first)))
        _read_sample(&second);

    _normalize_sample(&second);

    if (!_sample_valid(&second))
        return 0;

    int year = _full_year(&second);
    if (year < 1970)
        return 0;

    struct tm tm_val = {0};
    tm_val.tm_sec = second.sec;
    tm_val.tm_min = second.min;
    tm_val.tm_hour = second.hour;
    tm_val.tm_mday = second.mday;
    tm_val.tm_mon = second.mon - 1;
    tm_val.tm_year = year - 1900;
    tm_val.tm_isdst = 0;

    time_t unix_time = mktime(&tm_val);
    if (unix_time < 0)
        return 0;

    return (u64)unix_time;
}
