// cstd.fenv — <fenv.h> for Windows (UCRT / MSVCRT, built with mingw-w64).
//
// The exception flags and rounding modes take plain `int`s; the save/restore functions take `fenv_t`,
// whose layout is platform-specific and not exposed here. Windows uses the SSE control word, so the
// FE_* values are the MSVC ones (they differ from glibc's).

public unit cstd.fenv;

public const int FE_INEXACT = 1;
public const int FE_UNDERFLOW = 2;
public const int FE_OVERFLOW = 4;
public const int FE_DIVBYZERO = 8;
public const int FE_INVALID = 16;
public const int FE_ALL_EXCEPT = 31;

public const int FE_TONEAREST = 0;
public const int FE_UPWARD = 512;
public const int FE_DOWNWARD = 256;
public const int FE_TOWARDZERO = 768;

extern int feclearexcept(int excepts);
extern int fetestexcept(int excepts);
extern int feraiseexcept(int excepts);
extern int fegetround();
extern int fesetround(int mode);
