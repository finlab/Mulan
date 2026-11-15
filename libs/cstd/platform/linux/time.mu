// cstd.time — <time.h> for Linux (glibc).
//
// `time_t` and `clock_t` are both 64-bit here (unlike Windows' 32-bit `clock_t`). `struct tm` starts
// with the 9 standard ints and adds the two glibc extensions (`tm_gmtoff`, `tm_zone`), which are part
// of the struct the C library actually writes, so they must be declared for the layout to match.
//
// The POSIX reentrant variants (`localtime_r`, `gmtime_r`) and `timegm` exist here and are bound;
// they are the only usable form from a Mulan task, which may resume on another thread.

public unit cstd.time;

use std.ctypes;

public type Time = long;
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

extern Time time(Time* result);
extern Clock clock();
extern double difftime(Time later, Time earlier);
extern Time mktime(Tm* broken);
extern Tm* localtime(const Time* timer);
extern Tm* gmtime(const Time* timer);
extern char* asctime(const Tm* broken);
extern char* ctime(const Time* timer);
extern c_size strftime(char* buffer, c_size size, const char* format, const Tm* broken);

extern Tm* localtime_r(const Time* timer, Tm* result);
extern Tm* gmtime_r(const Time* timer, Tm* result);
extern Time timegm(Tm* broken);
