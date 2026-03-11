#include <locale.h>
#include <string.h>

static char current_locale[] = "C";

static struct lconv c_locale = {
    .decimal_point = ".",
    .thousands_sep = "",
    .grouping = "",
    .int_curr_symbol = "",
    .currency_symbol = "",
    .mon_decimal_point = "",
    .mon_thousands_sep = "",
    .mon_grouping = "",
    .positive_sign = "",
    .negative_sign = "",
    .int_frac_digits = (char)0xff,
    .frac_digits = (char)0xff,
    .p_cs_precedes = (char)0xff,
    .p_sep_by_space = (char)0xff,
    .n_cs_precedes = (char)0xff,
    .n_sep_by_space = (char)0xff,
    .p_sign_posn = (char)0xff,
    .n_sign_posn = (char)0xff,
};

char *setlocale(int category, const char *locale) {
    if (category < LC_ALL || category > LC_TIME) {
        return NULL;
    }

    if (!locale) {
        return current_locale;
    }

    if (!locale[0] || !strcmp(locale, "C") || !strcmp(locale, "POSIX")) {
        strcpy(current_locale, "C");
        return current_locale;
    }

    return NULL;
}

struct lconv *localeconv(void) {
    return &c_locale;
}
