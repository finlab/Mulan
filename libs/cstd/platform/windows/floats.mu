// cstd.float — <float.h> for Windows (UCRT / MSVCRT, built with mingw-w64).
//
// `long double` is the same as `double` on this platform, so the LDBL_* values equal the DBL_* ones
// (kept for source compatibility; Mulan has no `long double`).

public unit cstd.floats;

public const int FLT_RADIX = 2;
public const int FLT_MANT_DIG = 24;
public const int DBL_MANT_DIG = 53;
public const int LDBL_MANT_DIG = 53;

public const int FLT_DIG = 6;
public const int DBL_DIG = 15;
public const int LDBL_DIG = 15;

public const int FLT_MIN_EXP = -125;
public const int DBL_MIN_EXP = -1021;
public const int LDBL_MIN_EXP = -1021;

public const int FLT_MAX_EXP = 128;
public const int DBL_MAX_EXP = 1024;
public const int LDBL_MAX_EXP = 1024;

public const float FLT_MAX = 3.4028234663852886e+38 as float;
public const float FLT_MIN = 1.1754943508222875e-38;
public const float FLT_EPSILON = 1.1920928955078125e-07;

public const double DBL_MAX = 1.7976931348623157e+308;
public const double DBL_MIN = 2.2250738585072014e-308;
public const double DBL_EPSILON = 2.2204460492503131e-16;

public const double LDBL_MAX = 1.7976931348623157e+308;
public const double LDBL_MIN = 2.2250738585072014e-308;
public const double LDBL_EPSILON = 2.2204460492503131e-16;
