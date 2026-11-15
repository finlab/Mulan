// cstd.time — <time.h> for Windows (UCRT / MSVCRT, built with mingw-w64).
//
// `time_t` is 64-bit on this platform (the `_USE_32BIT_TIME_T` layout is not supported here); `clock_t`
// is C's 32-bit `long`. `struct tm` is the platform layout: 9 ints, 36 bytes. The POSIX-only additions
// (`tm_gmtoff`, `tm_zone`, `timegm`, `localtime_r`) do not exist on Windows and are not bound.

public unit cstd.time;

use std.ctypes;

public type Time = long;
public type Clock = c_long;

public const c_long CLOCKS_PER_SEC = 1000;

public struct Tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
}

extern Time time(Time* result);
extern Clock clock();
extern double difftime(Time later, Time earlier);
extern Time mktime(Tm* broken);
extern Tm* localtime(const Time* timer);
extern Tm* gmtime(const Time* timer);
extern char* asctime(const Tm* broken);
extern char* ctime(const Time* timer);
extern c_size strftime(char* buffer, c_size size, const char* format, const Tm* broken);
