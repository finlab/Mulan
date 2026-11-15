// cstd.stdlib — <stdlib.h> for Windows (UCRT / MSVCRT, built with mingw-w64).
//
// Raw binding (layer L1): C semantics, no checks, no exceptions, no ownership. `malloc`/`free` are
// here for interoperability; Mulan code allocates with `alloc`/`new`.
//
// Not bound: `quick_exit` / `at_quick_exit` / `aligned_alloc` (absent from both Windows CRTs).

public unit cstd.stdlib;

use std.ctypes;

// ---------------------------------------------------------------- constants
public const int EXIT_SUCCESS = 0;
public const int EXIT_FAILURE = 1;
public const int RAND_MAX = 32767;

// ---------------------------------------------------------------- memory
// `realloc`/`free` are language builtins (SPEC 8.1) and are not redeclared here. `strtold` is not
// bound either: Mulan has no `long double` (SPEC 3.1).
extern void* malloc(c_size size);
extern void* calloc(c_size count, c_size size);

// Windows 64-bit aligned allocation (there is no C11 aligned_alloc on Windows).
extern void* _aligned_malloc(c_size size, c_size alignment);
extern void _aligned_free(void* block);

// ---------------------------------------------------------------- process
extern void abort();
extern void exit(int status);
extern void _Exit(int status);
extern int atexit(() => void callback);
extern int system(const char* command);
extern char* getenv(const char* name);
extern int _putenv(const char* assignment);

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
