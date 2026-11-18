// cstd.streams — the three standard C streams.
//
// The handles are macros on Windows (`__acrt_iob_func(index)`) and data symbols on glibc
// (`extern FILE *stdout;`). This language cannot declare a variable and an accessor with the same name,
// and the Windows form is a function anyway, so both are wrapped as accessors here and `cstd.stdio`
// re-exposes them as the portable `stdin()` / `stdout()` / `stderr()`.
//
// The accessor must return the very FILE object the C runtime uses: wrapping fd 1 in a second FILE
// (`fdopen`) would create a second buffer on the same descriptor and scramble the output order.

public unit cstd.streams;

use std.comptime;   // `#if` reads the build constants; the `use` must precede the first `#if`
use std.ctypes;

#if os != OS.Windows and os != OS.Linux {
    #error "cstd.streams: no implementation for this platform (Windows and Linux only yet)"
}

#if os == OS.Windows {
    // `stdin`/`stdout`/`stderr` are macros over this function in the CRT headers.
    extern void* __acrt_iob_func(c_uint index);

    public void* stdin_handle() { return __acrt_iob_func(0); }
    public void* stdout_handle() { return __acrt_iob_func(1); }
    public void* stderr_handle() { return __acrt_iob_func(2); }
}
#elif os == OS.Linux {
    extern void* stdin;
    extern void* stdout;
    extern void* stderr;

    public void* stdin_handle() { return stdin; }
    public void* stdout_handle() { return stdout; }
    public void* stderr_handle() { return stderr; }
}
