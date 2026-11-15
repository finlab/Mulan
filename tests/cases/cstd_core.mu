// cstd core units: every declaration resolves at link time, and the common surface behaves.
//
// This case is restricted to the part of the binding surface that exists on every supported
// platform; the MSVC-only and POSIX-only symbols are covered by `cstd_windows` / `cstd_posix`, which
// carry an `@platform` marker. The C ABI itself (LP64 vs LLP64) shows up in the last output line.
unit cstd_core;

use cstd.ctype;
use cstd.errno;
use cstd.fenv;
use cstd.floats;
use cstd.inttypes;
use cstd.limits;
use cstd.locale;
use cstd.math;
use cstd.stdint;
use cstd.stdio;
use cstd.stdlib;
use cstd.string;
use cstd.time;
use std.ctypes;

int compare_int(const void* left, const void* right) {
    const int* a = left as const int*;
    const int* b = right as const int*;
    return *a - *b;
}

int main() {
    // ---------------------------------------------------------------- link coverage
    void*[30] stdlib_refs = {
        malloc as void*, calloc as void*, abort as void*, exit as void*, _Exit as void*,
        atexit as void*, system as void*, getenv as void*, strtod as void*,
        strtof as void*, strtol as void*, strtoul as void*, strtoll as void*,
        strtoull as void*, atoi as void*, atol as void*, atoll as void*,
        atof as void*, abs as void*, labs as void*, llabs as void*,
        rand as void*, srand as void*, qsort as void*, bsearch as void*,
        mblen as void*, mbtowc as void*, wctomb as void*, mbstowcs as void*,
        wcstombs as void*
    };
    void*[24] string_refs = {
        memcpy as void*, memmove as void*, memset as void*, memcmp as void*,
        memchr as void*, strlen as void*, strnlen as void*, strcmp as void*,
        strncmp as void*, strcoll as void*, strxfrm as void*, strcpy as void*,
        strncpy as void*, strcat as void*, strncat as void*, strchr as void*,
        strrchr as void*, strspn as void*, strcspn as void*, strpbrk as void*,
        strstr as void*, strtok as void*, strerror as void*, strdup as void*
    };
    void*[14] ctype_refs = {
        isalnum as void*, isalpha as void*, isblank as void*, iscntrl as void*,
        isdigit as void*, isgraph as void*, islower as void*, isprint as void*,
        ispunct as void*, isspace as void*, isupper as void*, isxdigit as void*,
        tolower as void*, toupper as void*
    };
    void*[71] math_refs = {
        fabs as void*, fmod as void*, remainder as void*, remquo as void*,
        fma as void*, fmax as void*, fmin as void*, fdim as void*,
        fabsf as void*, fmodf as void*, fmaxf as void*, fminf as void*,
        exp as void*, exp2 as void*, expm1 as void*, log as void*,
        log10 as void*, log1p as void*, log2 as void*, pow as void*,
        sqrt as void*, cbrt as void*, hypot as void*,
        expf as void*, logf as void*, powf as void*, sqrtf as void*,
        cbrtf as void*, hypotf as void*,
        sin as void*, cos as void*, tan as void*, asin as void*,
        acos as void*, atan as void*, atan2 as void*, sinh as void*,
        cosh as void*, tanh as void*, asinh as void*, acosh as void*,
        atanh as void*, sinf as void*, cosf as void*, tanf as void*,
        atan2f as void*, ceil as void*, floor as void*, trunc as void*,
        round as void*, lround as void*, llround as void*, nearbyint as void*,
        rint as void*, ceilf as void*, floorf as void*, truncf as void*,
        roundf as void*, frexp as void*, ldexp as void*, modf as void*,
        scalbn as void*, copysign as void*, nextafter as void*, frexpf as void*,
        ldexpf as void*, modff as void*, erf as void*, erfc as void*,
        tgamma as void*, lgamma as void*
    };
    void*[9] time_refs = {
        time as void*, clock as void*, difftime as void*, mktime as void*,
        localtime as void*, gmtime as void*, asctime as void*, ctime as void*,
        strftime as void*
    };
    void*[2] locale_refs = { setlocale as void*, localeconv as void* };
    void*[5] fenv_refs = {
        feclearexcept as void*, fetestexcept as void*, feraiseexcept as void*,
        fegetround as void*, fesetround as void*
    };
    void*[3] inttypes_refs = {
        imaxabs as void*, strtoimax as void*, strtoumax as void*
    };
    void*[1] errno_refs = { current as void* };

    // ---------------------------------------------------------------- string / ctype
    c_size length = cstd.string.strlen("hello");
    int ordered = strcmp("abc", "abd") < 0 ? 1 : 0;

    char[8] copy = {};
    char[3] source = {'a', 'b', 0};
    memcpy(&copy[0], &source[0], 3);
    int copied = strcmp(&copy[0], "ab") == 0 ? 1 : 0;

    char* found = strstr("one two three", "two");
    int found_ok = found != null and strcmp(found, "two three") == 0 ? 1 : 0;

    int digit = isdigit('7') != 0 ? 1 : 0;
    char upper = toupper('a') as char;

    // ---------------------------------------------------------------- stdlib
    int* block = malloc(sizeof(int) * 3) as int*;
    block[0] = 3;
    block[1] = 1;
    block[2] = 2;
    qsort(block, 3, sizeof(int), compare_int);
    int sorted_ok = block[0] == 1 and block[1] == 2 and block[2] == 3 ? 1 : 0;
    free(block);

    int* key = malloc(sizeof(int)) as int*;
    key[0] = 2;
    int[3] sorted = {1, 2, 3};
    int* hit = bsearch(key, &sorted[0], 3, sizeof(int), compare_int) as int*;
    int bsearch_ok = hit != null and *hit == 2 ? 1 : 0;
    free(key);

    double parsed = strtod("3.5", null);
    c_long integral = strtol("42", null, 10);
    long wide = imaxabs(-7 as long);
    IMaxDiv quot = imaxdiv(7, 2);
    int div_ok = quot.quot == 3 and quot.rem == 1 ? 1 : 0;

    // ---------------------------------------------------------------- math
    double root = sqrt(9.0);
    double power = pow(2.0, 10.0);
    double down = floor(-1.5);

    // ---------------------------------------------------------------- time round-trip
    // `Tm` carries platform extensions (glibc adds `tm_gmtoff`/`tm_zone`), so the aggregate is
    // zero-filled first and only the standard fields are assigned: a named initializer would have to
    // list the extension fields too.
    Tm broken = {};
    broken.tm_sec = 0;
    broken.tm_min = 0;
    broken.tm_hour = 0;
    broken.tm_mday = 1;
    broken.tm_mon = 0;
    broken.tm_year = 100;
    broken.tm_wday = 0;
    broken.tm_yday = 0;
    broken.tm_isdst = -1;
    Time stamp = mktime(&broken);
    Tm* back = localtime(&stamp);
    int year_ok = back != null and back->tm_year == 100 and back->tm_mon == 0 ? 1 : 0;
    int clock_base_ok = clock() >= 0 ? 1 : 0;

    // ---------------------------------------------------------------- errno / locale
    *current() = 0;
    *current() = EDOM;
    int errno_ok = *current() == EDOM ? 1 : 0;

    char* locale_name = setlocale(LC_ALL, "C");
    Lconv* conv = localeconv();
    int locale_ok = locale_name != null and conv != null and *conv->decimal_point == '.' ? 1 : 0;

    // ---------------------------------------------------------------- fenv
    int old_round = fegetround();
    fesetround(FE_UPWARD);
    int round_ok = fegetround() == FE_UPWARD ? 1 : 0;
    fesetround(old_round);
    feclearexcept(FE_ALL_EXCEPT);
    feraiseexcept(FE_INEXACT);
    int flags_ok = fetestexcept(FE_INEXACT) == FE_INEXACT ? 1 : 0;
    feclearexcept(FE_ALL_EXCEPT);

    // ---------------------------------------------------------------- inttypes
    char[32] formatted = {};
    char[16] int_fmt = {};
    sprintf(&int_fmt[0], "%%%s", PRId64);
    snprintf(&formatted[0], 32, &int_fmt[0], 1234567890123 as long);
    int format_ok = strcmp(&formatted[0], "1234567890123") == 0 ? 1 : 0;

    printf("len=%zu ordered=%d copied=%d found=%d digit=%d upper=%c sorted=%d bsearch=%d\n",
           length, ordered, copied, found_ok, digit, upper, sorted_ok, bsearch_ok);
    printf("parsed=%.1f integral=%ld wide=%lld root=%.1f power=%.1f down=%.1f\n",
           parsed, integral, wide, root, power, down);
    printf("year_ok=%d clock=%d errno=%d locale=%d round=%d flags=%d format=%d div=%d\n",
           year_ok, clock_base_ok, errno_ok, locale_ok, round_ok, flags_ok, format_ok, div_ok);
    printf("int_max=%d char_bit=%d dbl_dig=%d size_max=%zu refs=%zu\n", INT_MAX, CHAR_BIT, DBL_DIG,
           SIZE_MAX, sizeof(stdlib_refs) / sizeof(stdlib_refs[0]));
    return 0;
}
