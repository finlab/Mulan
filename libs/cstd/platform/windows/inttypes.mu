// cstd.inttypes — <inttypes.h> for Windows (UCRT / MSVCRT, built with mingw-w64).
//
// `imaxabs` / `imaxdiv` / `strtoimax` / `strtoumax` plus the printf format macros. The format strings
// follow what this platform's CRT accepts directly (`I64d`, not `lld`): a Mu program that opens a FILE
// through cstd.stdio hands the format to the CRT, not to MinGW's ANSI-stdio wrapper.

public unit cstd.inttypes;

use std.ctypes;

public type IMax = long;
public type UMax = ulong;

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
public const char* PRId64 = "I64d";

public const char* PRIi8 = "i";
public const char* PRIi16 = "i";
public const char* PRIi32 = "i";
public const char* PRIi64 = "I64i";

public const char* PRIu8 = "u";
public const char* PRIu16 = "u";
public const char* PRIu32 = "u";
public const char* PRIu64 = "I64u";

public const char* PRIx8 = "x";
public const char* PRIx16 = "x";
public const char* PRIx32 = "x";
public const char* PRIx64 = "I64x";
