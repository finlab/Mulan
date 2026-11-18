// cstd.errno — <errno.h>.
//
// `errno` is a macro over a per-platform accessor function, so it is exposed as an accessor returning
// the slot: `*cstd.errno.current() = 0;` reads or writes it. The value is thread-local, but a Mulan
// task may resume on another carrier, so errno must not be carried across a park (see the stdlib
// spec). Native APIs report failures through exceptions instead.
//
// The error numbers are the platform's ABI, not ISO C: the same name can have a different value on
// Windows and glibc.

public unit cstd.errno;

use std.comptime;   // `#if` reads the build constants; the `use` must precede the first `#if`
use std.ctypes;

#if os != OS.Windows and os != OS.Linux {
    #error "cstd.errno: no implementation for this platform (Windows and Linux only yet)"
}

public const int EDOM = 33;
public const int ERANGE = 34;
public const int EINVAL = 22;
public const int ENOMEM = 12;

#if os == OS.Windows {
    public const int EILSEQ = 42;

    extern int* _errno();

    public int* current() { return _errno(); }
}
#elif os == OS.Linux {
    public const int EILSEQ = 84;

    extern int* __errno_location();

    public int* current() { return __errno_location(); }
}
