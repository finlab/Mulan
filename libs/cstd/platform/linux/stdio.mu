// cstd.stdio — <stdio.h> for Linux (glibc).
//
// Raw binding (layer L1): C semantics, no checks, no exceptions, no ownership. The standard streams
// are data symbols on glibc, so they come from `cstd.streams` and appear here as the portable
// `stdin()` / `stdout()` / `stderr()` accessors.
//
// Not bound: the `v*printf` / `v*scanf` families (they need `va_list`, which the language does not
// provide, SPEC 7.1); the MSVC-only `_snprintf` / `_flushall`.

public unit cstd.stdio;

use cstd.streams;
use std.ctypes;

public type File = void*;

// ---------------------------------------------------------------- constants
public const int EOF = -1;
public const int BUFSIZ = 8192;
public const int FILENAME_MAX = 4096;
public const int FOPEN_MAX = 16;
public const int TMP_MAX = 238328;
public const int SEEK_SET = 0;
public const int SEEK_CUR = 1;
public const int SEEK_END = 2;
public const int _IOFBF = 0;
public const int _IOLBF = 1;
public const int _IONBF = 2;

// ---------------------------------------------------------------- streams
public File stdin() { return stdin_handle(); }
public File stdout() { return stdout_handle(); }
public File stderr() { return stderr_handle(); }

// ---------------------------------------------------------------- file operations
extern File fopen(const char* path, const char* mode);
extern File freopen(const char* path, const char* mode, File stream);
extern int fclose(File stream);
extern int fflush(File stream);
extern int remove(const char* path);
extern int rename(const char* old_path, const char* new_path);
extern File tmpfile();
extern char* tmpnam(char* buffer);
extern void setbuf(File stream, char* buffer);
extern int setvbuf(File stream, char* buffer, int mode, c_size size);

// ---------------------------------------------------------------- binary I/O
extern c_size fread(void* buffer, c_size size, c_size count, File stream);
extern c_size fwrite(const void* buffer, c_size size, c_size count, File stream);

// ---------------------------------------------------------------- formatted output
extern int printf(const char* format, ...);
extern int fprintf(File stream, const char* format, ...);
extern int sprintf(char* buffer, const char* format, ...);
extern int snprintf(char* buffer, c_size size, const char* format, ...);
extern int scanf(const char* format, ...);
extern int fscanf(File stream, const char* format, ...);
extern int sscanf(const char* buffer, const char* format, ...);

// ---------------------------------------------------------------- character / line
extern int fgetc(File stream);
extern int getc(File stream);
extern int getchar();
extern char* fgets(char* buffer, int size, File stream);
extern int fputc(int c, File stream);
extern int putc(int c, File stream);
extern int putchar(int c);
extern int fputs(const char* s, File stream);
extern int puts(const char* s);
extern int ungetc(int c, File stream);

// ---------------------------------------------------------------- positioning and status
// glibc's `fpos_t` is the byte offset plus an `mbstate_t` (16 bytes on x86-64). A scalar would be
// too small and `fgetpos` would write past the caller's object.
public struct Fpos {
    long offset;
    ulong state;
}

extern int fseek(File stream, c_long offset, int origin);
extern c_long ftell(File stream);
extern void rewind(File stream);
extern int fgetpos(File stream, Fpos* position);
extern int fsetpos(File stream, const Fpos* position);
extern void clearerr(File stream);
extern int feof(File stream);
extern int ferror(File stream);
extern void perror(const char* prefix);
extern int fileno(File stream);
