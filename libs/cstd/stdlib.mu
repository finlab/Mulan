// cstd.stdlib — <stdlib.h>.
//
// Raw binding (layer L1): C semantics, no checks, no exceptions, no ownership. `malloc`/`free` are here
// for interoperability; Mulan code allocates with `alloc`/`new` (`realloc`/`free` are language builtins,
// SPEC 8.1, and are not redeclared). `strtold` is not bound either: Mulan has no `long double`.
//
// Not bound: `quick_exit` / `at_quick_exit` (C11, above the C99 binding floor).
//
// Aligned allocation and the environment setters have no common spelling: Windows has
// `_aligned_malloc` / `_aligned_free` and `_putenv`, glibc has C11 `aligned_alloc` (alignment first,
// the opposite argument order) and POSIX `setenv` / `unsetenv`.

public unit cstd.stdlib;

use std.comptime;   // `#if` reads the build constants; the `use` must precede the first `#if`
use std.ctypes;

#if os != OS.Windows and os != OS.Linux {
    #error "cstd.stdlib: no implementation for this platform (Windows and Linux only yet)"
}

// ---------------------------------------------------------------- constants
public const int EXIT_SUCCESS = 0;
public const int EXIT_FAILURE = 1;

#if os == OS.Windows {
    public const int RAND_MAX = 32767;
}
#elif os == OS.Linux {
    public const int RAND_MAX = 2147483647;
}

// ---------------------------------------------------------------- memory
extern void* malloc(c_size size);
extern void* calloc(c_size count, c_size size);

#if os == OS.Windows {
    // Windows 64-bit aligned allocation (there is no C11 aligned_alloc on the Windows CRT).
    extern void* _aligned_malloc(c_size size, c_size alignment);
    extern void _aligned_free(void* block);
}
#elif os == OS.Linux {
    // C11 aligned allocation (glibc 2.16+).
    extern void* aligned_alloc(c_size alignment, c_size size);
}

// ---------------------------------------------------------------- process
extern void abort();
extern void exit(int status);
extern void _Exit(int status);
extern int atexit(() => void callback);
extern int system(const char* command);
extern char* getenv(const char* name);
#if os == OS.Windows {
    extern int _putenv(const char* assignment);
}
#elif os == OS.Linux {
    extern int setenv(const char* name, const char* value, int overwrite);
    extern int unsetenv(const char* name);
}

// ---------------------------------------------------------------- conversions
extern double strtod(const char* text, char** end);
extern float strtof(const char* text, char** end);
extern c_long strtol(const char* text, char** end, int base);
extern c_ulong strtoul(const char* text, char** end, int base);
extern c_longlong strtoll(const char* text, char** end, int base);
extern c_ulonglong strtoull(const char* text, char** end, int base);
extern int atoi(const char* text);
extern c_long atol(const char* text);
extern c_longlong atoll(const char* text);
extern double atof(const char* text);

// ---------------------------------------------------------------- arithmetic
extern int abs(int value);
extern c_long labs(c_long value);
extern c_longlong llabs(c_longlong value);
extern int rand();
extern void srand(c_uint seed);

// ---------------------------------------------------------------- search / sort
extern void qsort(void* base, c_size count, c_size size,
                  (const void*, const void*) => int compare);
extern void* bsearch(const void* key, const void* base, c_size count, c_size size,
                     (const void*, const void*) => int compare);

// ---------------------------------------------------------------- multibyte
extern int mblen(const char* s, c_size n);
extern int mbtowc(c_wchar* out, const char* s, c_size n);
extern int wctomb(char* s, c_wchar wide);
extern c_size mbstowcs(c_wchar* out, const char* s, c_size n);
extern c_size wcstombs(char* out, const c_wchar* wide, c_size n);
