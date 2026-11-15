// cstd.errno — <errno.h> for Windows (UCRT / MSVCRT, built with mingw-w64).
//
// `errno` is a macro over `*_errno()` in the CRT headers, so it is exposed as an accessor returning
// the slot: `*cstd.errno.current() = 0;` reads or writes it. The value is thread-local, but a Mulan
// task may resume on another carrier, so errno must not be carried across a park (see the stdlib
// spec). Native APIs report failures through exceptions instead.

public unit cstd.errno;

use std.ctypes;

public const int EDOM = 33;
public const int ERANGE = 34;
public const int EILSEQ = 42;
public const int EINVAL = 22;
public const int ENOMEM = 12;

extern int* _errno();

public int* current() { return _errno(); }
