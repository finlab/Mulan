// cstd.string — <string.h> for Windows (UCRT / MSVCRT, built with mingw-w64).
//
// Raw binding (layer L1): C semantics. `str*` functions return the destination or a pointer into the
// source; lengths are the caller's responsibility.

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

// ---------------------------------------------------------------- Windows extensions
extern char* _strdup(const char* s);
extern char* strdup(const char* s);
extern int _stricmp(const char* left, const char* right);
extern int _strnicmp(const char* left, const char* right, c_size count);
extern char* strtok_s(char* s, const char* delimiters, char** context);
extern int strerror_s(char* buffer, c_size size, int error_number);

// ---------------------------------------------------------------- memory duplication
extern void* _expand(void* block, c_size size);
extern c_size _msize(void* block);
