// cstdio.mu — bindings to the C standard library <stdio.h> (ABI aligned with msvcrt / UCRT)
//
// declarations only, no implementation: the linker resolves the symbols from the C runtime.
// `FILE` is an opaque struct on the C side, uniformly represented as `void*` here; `...` marks C variadic
// parameters (arguments follow the C default argument promotion rules, and `string` converts to `const char*`).
//
// not bound: the `vprintf` family (needs `va_list`), `fgetpos`/`fsetpos` (need `fpos_t`),
// `getline` (a POSIX extension), the `%n` semantics of `scanf`.

public unit std.cstdio;

use std.ctypes;

// ---------------------------------------------------------------- constants
public const int EOF = -1;
public const int BUFSIZ = 512;
public const int FILENAME_MAX = 260;
public const int SEEK_SET = 0;
public const int SEEK_CUR = 1;
public const int SEEK_END = 2;

// ---------------------------------------------------------------- file operations
extern void* fopen(const char* path, const char* mode);
extern void* freopen(const char* path, const char* mode, void* stream);
extern int fclose(void* stream);
extern int fflush(void* stream);
extern int remove(const char* path);
extern int rename(const char* old_path, const char* new_path);
extern void* tmpfile();
// Binary I/O: `size` is C's `size_t`, the count is `size_t` too (SPEC 7.2)
extern c_size fread(void* buf, c_size size, c_size n, void* stream);
extern c_size fwrite(const void* buf, c_size size, c_size n, void* stream);

// ---------------------------------------------------------------- formatted output
extern int printf(const char* fmt, ...);
extern int fprintf(void* stream, const char* fmt, ...);
extern int sprintf(char* buf, const char* fmt, ...);
extern int snprintf(char* buf, usize size, const char* fmt, ...);

// ---------------------------------------------------------------- character / line
extern int fgetc(void* stream);
extern int getchar();
extern char* fgets(char* buf, int size, void* stream);
extern int fputc(int c, void* stream);
extern int putchar(int c);
extern int fputs(const char* s, void* stream);
extern int puts(const char* s);
extern int ungetc(int c, void* stream);

// ---------------------------------------------------------------- formatted input
extern int scanf(const char* fmt, ...);
extern int fscanf(void* stream, const char* fmt, ...);
extern int sscanf(const char* s, const char* fmt, ...);

// ---------------------------------------------------------------- positioning
extern int fseek(void* stream, c_long offset, int origin);
extern c_long ftell(void* stream);
extern void rewind(void* stream);

// ---------------------------------------------------------------- status and errors
extern void clearerr(void* stream);
extern int feof(void* stream);
extern int ferror(void* stream);
extern void perror(const char* s);
