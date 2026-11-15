// cstd.errno — <errno.h> for Linux (glibc).
//
// `errno` is a macro over `*__errno_location()` in glibc's headers, so it is exposed as an accessor
// returning the slot: `*cstd.errno.current() = 0;` reads or writes it. The value is thread-local, but a
// Mulan task may resume on another carrier, so errno must not be carried across a park (see the stdlib
// spec). Native APIs report failures through exceptions instead.
//
// The error numbers are glibc's (platform ABI, not ISO C) and differ from the Windows values for the
// same names.

public unit cstd.errno;

use std.ctypes;

public const int EDOM = 33;
public const int ERANGE = 34;
public const int EILSEQ = 84;
public const int EINVAL = 22;
public const int ENOMEM = 12;

extern int* __errno_location();

public int* current() { return __errno_location(); }
