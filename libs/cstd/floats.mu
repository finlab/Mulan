// cstd.floats — <float.h>.
//
// Mulan has no `long double`, so `FLT_*` and `DBL_*` are the usable surface. The `LDBL_*` precision and
// exponent constants report what the platform's `long double` actually is (`double` on Windows, the
// 80-bit x87 format on x86-64 glibc); the `LDBL_MAX` / `LDBL_MIN` / `LDBL_EPSILON` values cannot be
// represented as a `double` on glibc, so they carry the `double` values and exist for source
// compatibility only.

public unit cstd.floats;

use std.comptime;   // `#if` reads the build constants; the `use` must precede the first `#if`

#if os != OS.Windows and os != OS.Linux {
    #error "cstd.floats: no implementation for this platform (Windows and Linux only yet)"
}

public const int FLT_RADIX = 2;
public const int FLT_MANT_DIG = 24;
public const int DBL_MANT_DIG = 53;

public const int FLT_DIG = 6;
public const int DBL_DIG = 15;

public const int FLT_MIN_EXP = -125;
public const int DBL_MIN_EXP = -1021;

public const int FLT_MAX_EXP = 128;
public const int DBL_MAX_EXP = 1024;

#if os == OS.Windows {
    public const int LDBL_MANT_DIG = 53;
    public const int LDBL_DIG = 15;
    public const int LDBL_MIN_EXP = -1021;
    public const int LDBL_MAX_EXP = 1024;
}
#elif os == OS.Linux {
    public const int LDBL_MANT_DIG = 64;
    public const int LDBL_DIG = 18;
    public const int LDBL_MIN_EXP = -16381;
    public const int LDBL_MAX_EXP = 16384;
}

public const float FLT_MAX = 3.4028234663852886e+38 as float;
public const float FLT_MIN = 1.1754943508222875e-38;
public const float FLT_EPSILON = 1.1920928955078125e-07;

public const double DBL_MAX = 1.7976931348623157e+308;
public const double DBL_MIN = 2.2250738585072014e-308;
public const double DBL_EPSILON = 2.2204460492503131e-16;

public const double LDBL_MAX = 1.7976931348623157e+308;
public const double LDBL_MIN = 2.2250738585072014e-308;
public const double LDBL_EPSILON = 2.2204460492503131e-16;
