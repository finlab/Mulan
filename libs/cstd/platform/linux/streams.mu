// cstd.streams — the standard C streams (Linux, glibc)
//
// glibc exposes `stdin`/`stdout`/`stderr` as data symbols (`extern FILE *stdout;`), while this
// language cannot declare a variable and an accessor with the same name. The symbols stay private
// here and `cstd.stdio` re-exposes them as the portable `stdout()` API.
//
// The accessor must return the very FILE object the C runtime uses: wrapping fd 1 in a second FILE
// (`fdopen`) would create a second buffer on the same descriptor and scramble the output order.

public unit cstd.streams;

extern void* stdin;
extern void* stdout;
extern void* stderr;

public void* stdin_handle() { return stdin; }
public void* stdout_handle() { return stdout; }
public void* stderr_handle() { return stderr; }
