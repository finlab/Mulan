// cstd.locale — <locale.h>.
//
// Locale names and the meaning of `setlocale` are platform-specific ("C" is the only portable value).
// `setlocale` mutates process-global state, so it is not usable from a task that may migrate between
// threads. The category constants are the platform's numbering, not ISO C's (ISO C fixes the names, not
// the values), and only glibc has `LC_MESSAGES`.
//
// `struct lconv` has the standard fields in the standard order on both platforms; each C library
// appends its own fields after `n_sign_posn` (wide fields on Windows, `int_p_*` on glibc). Only the
// standard prefix is declared: the object is read-only and only that prefix is accessed.

public unit cstd.locale;

use std.comptime;   // `#if` reads the build constants; the `use` must precede the first `#if`
use std.ctypes;

#if os != OS.Windows and os != OS.Linux {
    #error "cstd.locale: no implementation for this platform (Windows and Linux only yet)"
}

public const int LC_MIN = 0;
public const int LC_MAX = 5;

#if os == OS.Windows {
    public const int LC_ALL = 0;
    public const int LC_COLLATE = 1;
    public const int LC_CTYPE = 2;
    public const int LC_MONETARY = 3;
    public const int LC_NUMERIC = 4;
    public const int LC_TIME = 5;
}
#elif os == OS.Linux {
    public const int LC_CTYPE = 0;
    public const int LC_NUMERIC = 1;
    public const int LC_TIME = 2;
    public const int LC_COLLATE = 3;
    public const int LC_MONETARY = 4;
    public const int LC_MESSAGES = 5;
    public const int LC_ALL = 6;
}

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
