// cstd.inttypes — <inttypes.h> for Linux (glibc, x86-64).
//
// `imaxabs` / `imaxdiv` / `strtoimax` / `strtoumax` plus the printf format macros. glibc follows
// the C standard here: `long` is 64-bit (LP64), so the 64-bit formats are `ld` / `li` / `lu` /
// `lx` — not the `I64d` family the Windows CRT expects.

public unit cstd.inttypes;

use std.ctypes;

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
public const char* PRId64 = "ld";

public const char* PRIi8 = "i";
public const char* PRIi16 = "i";
public const char* PRIi32 = "i";
public const char* PRIi64 = "li";

public const char* PRIu8 = "u";
public const char* PRIu16 = "u";
public const char* PRIu32 = "u";
public const char* PRIu64 = "lu";

public const char* PRIx8 = "x";
public const char* PRIx16 = "x";
public const char* PRIx32 = "x";
public const char* PRIx64 = "lx";
