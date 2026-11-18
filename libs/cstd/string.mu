// cstd.string — <string.h>.
//
// Raw binding (layer L1): C semantics. `str*` functions return the destination or a pointer into the
// source; lengths are the caller's responsibility.
//
// The ISO C surface is identical on both platforms; only the extensions below differ. Windows binds the
// MSVC spellings (`_strdup`, `_stricmp`, `_strnicmp`, `strtok_s`, `strerror_s`, `_expand`, `_msize`)
// plus `strdup`; glibc has the POSIX ones (`strdup`, `strndup`, `strtok_r`, `strcasecmp`,
// `strncasecmp`).
//
// `strtok` keeps its cursor in one global on both platforms, so concurrent tokenization corrupts it; use
// the reentrant variant with a caller-owned context when more than one thread tokenizes.

public unit cstd.string;

use std.comptime;   // `#if` reads the build constants; the `use` must precede the first `#if`
use std.ctypes;

#if os != OS.Windows and os != OS.Linux {
    #error "cstd.string: no implementation for this platform (Windows and Linux only yet)"
}

// ---------------------------------------------------------------- block operations
extern void* memcpy(void* destination, const void* source, c_size count);
extern void* memmove(void* destination, const void* source, c_size count);
extern void* memset(void* destination, int value, c_size count);
extern int memcmp(const void* left, const void* right, c_size count);
extern void* memchr(const void* block, int value, c_size count);

// ---------------------------------------------------------------- length and comparison
extern c_size strlen(const char* s);
extern c_size strnlen(const char* s, c_size maximum);
extern int strcmp(const char* left, const char* right);
extern int strncmp(const char* left, const char* right, c_size count);
extern int strcoll(const char* left, const char* right);
extern c_size strxfrm(char* destination, const char* source, c_size count);

// ---------------------------------------------------------------- copy and concatenate
extern char* strcpy(char* destination, const char* source);
extern char* strncpy(char* destination, const char* source, c_size count);
extern char* strcat(char* destination, const char* source);
extern char* strncat(char* destination, const char* source, c_size count);

// ---------------------------------------------------------------- search
extern char* strchr(const char* s, int c);
extern char* strrchr(const char* s, int c);
extern c_size strspn(const char* s, const char* accept);
extern c_size strcspn(const char* s, const char* reject);
extern char* strpbrk(const char* s, const char* accept);
extern char* strstr(const char* haystack, const char* needle);
extern char* strtok(char* s, const char* delimiters);

// ---------------------------------------------------------------- errors
extern char* strerror(int error_number);

// ---------------------------------------------------------------- extensions
#if os == OS.Windows {
    extern char* _strdup(const char* s);
    extern char* strdup(const char* s);
    extern int _stricmp(const char* left, const char* right);
    extern int _strnicmp(const char* left, const char* right, c_size count);
    extern char* strtok_s(char* s, const char* delimiters, char** context);
    extern int strerror_s(char* buffer, c_size size, int error_number);

    // Heap introspection for a block obtained from malloc/calloc.
    extern void* _expand(void* block, c_size size);
    extern c_size _msize(void* block);
}
#elif os == OS.Linux {
    extern char* strdup(const char* s);
    extern char* strndup(const char* s, c_size n);
    extern char* strtok_r(char* s, const char* delimiters, char** context);
    extern int strcasecmp(const char* left, const char* right);
    extern int strncasecmp(const char* left, const char* right, c_size count);
}
