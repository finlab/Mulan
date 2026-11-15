// cstd.stdlib — <stdlib.h> for Linux (glibc, LP64).
//
// Raw binding (layer L1): C semantics, no checks, no exceptions, no ownership. `malloc`/`free` are
// here for interoperability; Mulan code allocates with `alloc`/`new`.
//
// Not bound: `quick_exit` / `at_quick_exit` (C11, above the C99 binding floor).
//
// The MSVC-only `_putenv` does not exist in glibc; the POSIX setters `setenv` / `unsetenv` are
// bound instead. Aligned allocation is C11 `aligned_alloc` here, not `_aligned_malloc`.

public unit cstd.stdlib;

use std.ctypes;

// ---------------------------------------------------------------- constants
public const int EXIT_SUCCESS = 0;
public const int EXIT_FAILURE = 1;
public const int RAND_MAX = 2147483647;

// ---------------------------------------------------------------- memory
// `realloc`/`free` are language builtins (SPEC 8.1) and are not redeclared here. `strtold` is not
// bound either: Mulan has no `long double` (SPEC 3.1).
extern void* malloc(c_size size);
extern void* calloc(c_size count, c_size size);

// C11 aligned allocation (glibc 2.16+). `alignment` comes first, the opposite of `_aligned_malloc`.
extern void* aligned_alloc(c_size alignment, c_size size);

// ---------------------------------------------------------------- process
extern void abort();
extern void exit(int status);
extern void _Exit(int status);
extern int atexit(() => void callback);
extern int system(const char* command);
extern char* getenv(const char* name);
extern int setenv(const char* name, const char* value, int overwrite);
extern int unsetenv(const char* name);

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
