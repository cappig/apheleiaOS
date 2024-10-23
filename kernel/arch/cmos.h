#include <base/types.h>
#include <time.h>

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

typedef struct tm std_time;


std_time get_time(void);
