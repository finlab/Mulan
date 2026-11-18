// cstd.fenv — <fenv.h>.
//
// The exception flags and rounding modes take plain `int`s; the save/restore functions take `fenv_t`,
// whose layout is platform-specific and not exposed here. On x86-64 both platforms encode these in the
// SSE control word, but the `FE_*` values are the C library's own numbering and differ between MSVC's
// CRT and glibc.

public unit cstd.fenv;

use std.comptime;   // `#if` reads the build constants; the `use` must precede the first `#if`

#if os != OS.Windows and os != OS.Linux {
    #error "cstd.fenv: no implementation for this platform (Windows and Linux only yet)"
}

public const int FE_TONEAREST = 0;

#if os == OS.Windows {
    public const int FE_INEXACT = 1;
    public const int FE_UNDERFLOW = 2;
    public const int FE_OVERFLOW = 4;
    public const int FE_DIVBYZERO = 8;
    public const int FE_INVALID = 16;
    public const int FE_ALL_EXCEPT = 31;

    public const int FE_UPWARD = 512;
    public const int FE_DOWNWARD = 256;
    public const int FE_TOWARDZERO = 768;
}
#elif os == OS.Linux {
    public const int FE_INVALID = 1;
    public const int FE_DIVBYZERO = 4;
    public const int FE_OVERFLOW = 8;
    public const int FE_UNDERFLOW = 16;
    public const int FE_INEXACT = 32;
    public const int FE_ALL_EXCEPT = 61;

    public const int FE_DOWNWARD = 0x400;
    public const int FE_UPWARD = 0x800;
    public const int FE_TOWARDZERO = 0xC00;
}

extern int feclearexcept(int excepts);
extern int fetestexcept(int excepts);
extern int feraiseexcept(int excepts);
extern int fegetround();
extern int fesetround(int mode);
