// cstd.stdint — <stdint.h> constants for Windows (UCRT / MSVCRT, built with mingw-w64).
//
// The *types* live in `std.ctypes` (c_int, c_long, ...); this unit adds the limit macros as
// compile-time constants. Function-like macros (`INT64_C`, ...) are not bound: Mulan has typed
// literals and casts, so they have no job here.

public unit cstd.stdint;

use std.ctypes;

public const int INT8_MIN = -128;
public const int INT8_MAX = 127;
public const int UINT8_MAX = 255;

public const int INT16_MIN = -32768;
public const int INT16_MAX = 32767;
public const int UINT16_MAX = 65535;

public const int INT32_MIN = -2147483647 - 1;
public const int INT32_MAX = 2147483647;
public const uint UINT32_MAX = 0xFFFFFFFF;

public const long INT64_MIN = -9223372036854775807 - 1;
public const long INT64_MAX = 9223372036854775807;
public const ulong UINT64_MAX = (0 as ulong) - 1;

public const c_ssize INTPTR_MIN = (-9223372036854775807 - 1) as c_ssize;
public const c_ssize INTPTR_MAX = 9223372036854775807 as c_ssize;
public const c_size UINTPTR_MAX = (0 as c_size) - 1;
public const c_ssize PTRDIFF_MIN = (-9223372036854775807 - 1) as c_ssize;
public const c_ssize PTRDIFF_MAX = 9223372036854775807 as c_ssize;
public const c_size SIZE_MAX = (0 as c_size) - 1;

public const long INTMAX_MIN = -9223372036854775807 - 1;
public const long INTMAX_MAX = 9223372036854775807;
public const ulong UINTMAX_MAX = (0 as ulong) - 1;

public const int SIG_ATOMIC_MIN = -2147483647 - 1;
public const int SIG_ATOMIC_MAX = 2147483647;
