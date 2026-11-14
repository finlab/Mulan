// std.ctypes — C ABI types, per platform
//
// Mu's integer widths are fixed (`long` is always 64-bit), while C's `long` is 32-bit on Windows
// (LLP64) and 64-bit on POSIX (LP64). Declaring an `extern` with Mu's `long` there silently changes
// the ABI — the historical `fseek`/`ftell` bug. Use these names for every C declaration:
//
//     use std.ctypes;
//     extern c_long ftell(void* stream);
//
// This unit is platform-specific (``libs/std/platform/<os>/std/ctypes.mu``); the names are the same
// everywhere, only the widths behind them change.

public unit std.ctypes;

public type c_char = char;
public type c_schar = char;
public type c_uchar = byte;
public type c_short = short;
public type c_ushort = ushort;
public type c_int = int;
public type c_uint = uint;
public type c_long = long;  // LP64: C `long` is 64-bit
public type c_ulong = ulong;
public type c_longlong = long;
public type c_ulonglong = ulong;
public type c_float = float;
public type c_double = double;
public type c_size = usize;    // size_t
public type c_ssize = isize;   // ssize_t / ptrdiff_t
public type c_intptr = isize;
public type c_uintptr = usize;
public type c_wchar = uint;  // wchar_t is 32-bit
public type c_voidp = void*;
