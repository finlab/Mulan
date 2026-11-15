// cstd.fenv — <fenv.h> for Linux (glibc).
//
// The exception flags and rounding modes take plain `int`s; the save/restore functions take `fenv_t`,
// whose layout is platform-specific and not exposed here. On x86-64 the FE_* values are encoded in the
// SSE control word and are glibc's (they differ from the MSVC/Windows values).

public unit cstd.fenv;

public const int FE_INVALID = 1;
public const int FE_DIVBYZERO = 4;
public const int FE_OVERFLOW = 8;
public const int FE_UNDERFLOW = 16;
public const int FE_INEXACT = 32;
public const int FE_ALL_EXCEPT = 61;

public const int FE_TONEAREST = 0;
public const int FE_DOWNWARD = 0x400;
public const int FE_UPWARD = 0x800;
public const int FE_TOWARDZERO = 0xC00;

extern int feclearexcept(int excepts);
extern int fetestexcept(int excepts);
extern int feraiseexcept(int excepts);
extern int fegetround();
extern int fesetround(int mode);
