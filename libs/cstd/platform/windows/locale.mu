// cstd.locale — <locale.h> for Windows (UCRT / MSVCRT, built with mingw-w64).
//
// Locale names and the meaning of `setlocale` are platform-specific ("C" is the only portable value);
// `setlocale` mutates process-global state, so it is not usable from a task that may migrate between
// threads. `struct lconv` has the standard fields in the standard order followed by Windows-only wide
// fields; only the standard prefix is declared, and it is read-only, so the prefix is enough.

public unit cstd.locale;

use std.ctypes;

public const int LC_ALL = 0;
public const int LC_COLLATE = 1;
public const int LC_CTYPE = 2;
public const int LC_MONETARY = 3;
public const int LC_NUMERIC = 4;
public const int LC_TIME = 5;
public const int LC_MIN = 0;
public const int LC_MAX = 5;

public struct Lconv {
    char* decimal_point;
    char* thousands_sep;
    char* grouping;
    char* int_curr_symbol;
    char* currency_symbol;
    char* mon_decimal_point;
    char* mon_thousands_sep;
    char* mon_grouping;
    char* positive_sign;
    char* negative_sign;
    char int_frac_digits;
    char frac_digits;
    char p_cs_precedes;
    char p_sep_by_space;
    char n_cs_precedes;
    char n_sep_by_space;
    char p_sign_posn;
    char n_sign_posn;
}

extern char* setlocale(int category, const char* locale);
extern Lconv* localeconv();
