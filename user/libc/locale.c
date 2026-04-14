#include "ulib.h"

/* Fixed "C" locale — setlocale always returns "C", localeconv returns
   a POSIX-default lconv. Lua calls these a handful of times at startup
   and for number formatting. */

struct lconv {
    char *decimal_point;
    char *thousands_sep;
    char *grouping;
    char *int_curr_symbol;
    char *currency_symbol;
    char *mon_decimal_point;
    char *mon_thousands_sep;
    char *mon_grouping;
    char *positive_sign;
    char *negative_sign;
    char  int_frac_digits;
    char  frac_digits;
    char  p_cs_precedes;
    char  p_sep_by_space;
    char  n_cs_precedes;
    char  n_sep_by_space;
    char  p_sign_posn;
    char  n_sign_posn;
};

static struct lconv c_lconv = {
    ".", "", "",
    "", "", ".", "", "",
    "", "-",
    127, 127, 127, 127, 127, 127, 127, 127
};

char *setlocale(int category, const char *locale) {
    (void)category;
    (void)locale;
    return (char *)"C";
}

struct lconv *localeconv(void) {
    return &c_lconv;
}
