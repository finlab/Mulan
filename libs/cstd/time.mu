// cstd.time — <time.h>.
//
// `time_t` is 64-bit on both platforms (the Windows `_USE_32BIT_TIME_T` layout is not supported).
// `clock_t` is not: C makes it `long`, which is 32-bit under LLP64 and 64-bit under LP64, and
// `CLOCKS_PER_SEC` follows the same split. `struct tm` starts with the nine standard `int` fields on
// both; glibc appends `tm_gmtoff` / `tm_zone`, which the library actually writes, so they are declared
// for the layout to match.
//
// The POSIX reentrant variants (`localtime_r`, `gmtime_r`) and `timegm` exist only on glibc. They are
// the only usable form from a Mulan task, which may resume on another thread.

public unit cstd.time;

use std.comptime;   // `#if` reads the build constants; the `use` must precede the first `#if`
use std.ctypes;

#if os != OS.Windows and os != OS.Linux {
    #error "cstd.time: no implementation for this platform (Windows and Linux only yet)"
}

public type Time = long;

#if os == OS.Windows {
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
}
#elif os == OS.Linux {
    public type Clock = long;

    public const c_long CLOCKS_PER_SEC = 1000000;

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
        long tm_gmtoff;       // seconds east of UTC
        const char* tm_zone;  // timezone abbreviation
    }
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

#if os == OS.Linux {
    extern Tm* localtime_r(const Time* timer, Tm* result);
    extern Tm* gmtime_r(const Time* timer, Tm* result);
    extern Time timegm(Tm* broken);
}
