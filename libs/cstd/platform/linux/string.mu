// cstd.string — <string.h> for Linux (glibc, LP64).
//
// Raw binding (layer L1): C semantics. `str*` functions return the destination or a pointer into the
// source; lengths are the caller's responsibility.
//
// The MSVC extensions of the Windows binding (`_strdup`, `_stricmp`, `_strnicmp`, `strtok_s`,
// `strerror_s`, `_expand`, `_msize`) do not exist in glibc and are therefore not bound; the POSIX
// spellings (`strdup`, `strndup`, `strtok_r`, `strcasecmp`, `strncasecmp`) are bound instead.
//
// `strtok` keeps its cursor in one global on glibc, so concurrent tokenization corrupts it; use
// `strtok_r` with a caller-owned context when more than one thread tokenizes.

public unit cstd.string;

use std.ctypes;

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

// ---------------------------------------------------------------- POSIX extensions
extern char* strdup(const char* s);
extern char* strndup(const char* s, c_size n);
extern char* strtok_r(char* s, const char* delimiters, char** context);
extern int strcasecmp(const char* left, const char* right);
extern int strncasecmp(const char* left, const char* right, c_size count);
