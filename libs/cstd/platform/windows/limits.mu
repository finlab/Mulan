// cstd.limits — <limits.h> for Windows (UCRT / MSVCRT, built with mingw-w64).
//
// The values are the platform's: `char` is signed, `long` is 32-bit (LLP64), `long long` is 64-bit.
// Integer limits are typed with the matching `c_*` name so that a comparison cannot silently widen.

public unit cstd.limits;

use std.ctypes;

public const int CHAR_BIT = 8;
public const int SCHAR_MIN = -128;
public const int SCHAR_MAX = 127;
public const int UCHAR_MAX = 255;
public const int CHAR_MIN = -128;
public const int CHAR_MAX = 127;
public const int MB_LEN_MAX = 5;

public const int SHRT_MIN = -32768;
public const int SHRT_MAX = 32767;
public const int USHRT_MAX = 65535;

public const int INT_MIN = -2147483647 - 1;
public const int INT_MAX = 2147483647;
public const uint UINT_MAX = 0xFFFFFFFF;

public const c_long LONG_MIN = -2147483647 - 1;
public const c_long LONG_MAX = 2147483647;
public const c_ulong ULONG_MAX = 0xFFFFFFFF;

public const long LLONG_MIN = -9223372036854775807 - 1;
public const long LLONG_MAX = 9223372036854775807;
public const ulong ULLONG_MAX = (0 as ulong) - 1;
