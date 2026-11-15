// cstd.floats — <float.h> for Linux (glibc, x86-64).
//
// x86-64 glibc's `long double` is the 80-bit x87 extended format, so the LDBL_* precision and
// exponent constants below are the real ones. Mulan has no `long double`: `LDBL_MAX`, `LDBL_MIN`
// and `LDBL_EPSILON` cannot be represented, so they carry the `double` values and exist only for
// source compatibility.

public unit cstd.floats;

public const int FLT_RADIX = 2;
public const int FLT_MANT_DIG = 24;
public const int DBL_MANT_DIG = 53;
public const int LDBL_MANT_DIG = 64;

public const int FLT_DIG = 6;
public const int DBL_DIG = 15;
public const int LDBL_DIG = 18;

public const int FLT_MIN_EXP = -125;
public const int DBL_MIN_EXP = -1021;
public const int LDBL_MIN_EXP = -16381;

public const int FLT_MAX_EXP = 128;
public const int DBL_MAX_EXP = 1024;
public const int LDBL_MAX_EXP = 16384;

public const float FLT_MAX = 3.4028234663852886e+38 as float;
public const float FLT_MIN = 1.1754943508222875e-38;
public const float FLT_EPSILON = 1.1920928955078125e-07;

public const double DBL_MAX = 1.7976931348623157e+308;
public const double DBL_MIN = 2.2250738585072014e-308;
public const double DBL_EPSILON = 2.2204460492503131e-16;

// The real 80-bit values do not fit a `double`; these are the `double` ones, for source
// compatibility only.
public const double LDBL_MAX = 1.7976931348623157e+308;
public const double LDBL_MIN = 2.2250738585072014e-308;
public const double LDBL_EPSILON = 2.2204460492503131e-16;
