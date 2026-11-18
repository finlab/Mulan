// cstd.inttypes — <inttypes.h>.
//
// `imaxabs` / `imaxdiv` / `strtoimax` / `strtoumax` plus the printf format macros. The 64-bit format
// strings are the one thing that must match the platform's CRT: glibc follows C (`ld` / `li` / `lu` /
// `lx`, because `long` is 64-bit there), while the Windows CRT wants the `I64d` family. A Mu program
// that opens a FILE through `cstd.stdio` hands the format to the CRT, not to MinGW's ANSI-stdio
// wrapper, so the Windows values are the `I64*` ones.

public unit cstd.inttypes;

use std.comptime;   // `#if` reads the build constants; the `use` must precede the first `#if`
use std.ctypes;

#if os != OS.Windows and os != OS.Linux {
    #error "cstd.inttypes: no implementation for this platform (Windows and Linux only yet)"
}

public type IMax = c_longlong;
public type UMax = c_ulonglong;

public struct IMaxDiv {
    IMax quot;
    IMax rem;
}

extern IMax imaxabs(IMax value);
extern IMaxDiv imaxdiv(IMax numerator, IMax denominator);
extern IMax strtoimax(const char* text, char** end, int base);
extern UMax strtoumax(const char* text, char** end, int base);

public const char* PRId8 = "d";
public const char* PRId16 = "d";
public const char* PRId32 = "d";

public const char* PRIi8 = "i";
public const char* PRIi16 = "i";
public const char* PRIi32 = "i";

public const char* PRIu8 = "u";
public const char* PRIu16 = "u";
public const char* PRIu32 = "u";

public const char* PRIx8 = "x";
public const char* PRIx16 = "x";
public const char* PRIx32 = "x";

#if os == OS.Windows {
    public const char* PRId64 = "I64d";
    public const char* PRIi64 = "I64i";
    public const char* PRIu64 = "I64u";
    public const char* PRIx64 = "I64x";
}
#elif os == OS.Linux {
    public const char* PRId64 = "ld";
    public const char* PRIi64 = "li";
    public const char* PRIu64 = "lu";
    public const char* PRIx64 = "lx";
}
