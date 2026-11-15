// cstd.stdio: every declaration resolves at link time, and the core file API round-trips.
//
// Restricted to the portable surface: the MSVC-only `__acrt_iob_func` / `_flushall` / `_snprintf`
// are covered by `cstd_windows`, the POSIX additions by `cstd_posix`. `BUFSIZ` really differs between
// the CRTs, so the expected output has a per-OS file.
unit cstd_stdio;

use cstd.stdio;
use std.ctypes;

int never = 0;

// Variadic declarations cannot be used as values, so they are called from a branch that never runs:
// the calls still force the linker to resolve them.
void variadic_refs(char* buffer) {
    int v = 0;
    if (never != 0) {
        scanf("%d", &v);
        fscanf(stdout(), "%d", &v);
        fprintf(stdout(), "%d", v);
        sprintf(buffer, "%d", v);
        sscanf("42", "%d", &v);
        snprintf(buffer, 8, "%d", v);
    }
}

int main() {
    // One reference per non-variadic declaration: a missing symbol fails the link.
    void*[32] refs = {
        fopen as void*, freopen as void*, fclose as void*, fflush as void*,
        remove as void*, rename as void*, tmpfile as void*,
        tmpnam as void*, setbuf as void*, setvbuf as void*, fread as void*,
        fwrite as void*, fgetc as void*, getc as void*,
        getchar as void*, fgets as void*, fputc as void*, putc as void*,
        putchar as void*, fputs as void*, puts as void*, ungetc as void*,
        perror as void*,
        fseek as void*, ftell as void*, rewind as void*, fgetpos as void*,
        fsetpos as void*, clearerr as void*, feof as void*, ferror as void*,
        fileno as void*
    };

    char[64] scratch = {};
    variadic_refs(&scratch[0]);

    if (stdout() == null or stdin() == null or stderr() == null) {
        return 1;
    }

    File f = tmpfile();
    if (f == null) {
        return 2;
    }
    char[6] payload = {'h', 'e', 'l', 'l', 'o', 0};
    c_size written = fwrite(&payload[0], 1, 5, f);
    if (written != 5) {
        return 3;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        return 4;
    }
    char[16] buffer = {};
    c_size got = fread(&buffer[0], 1, 5, f);
    fclose(f);

    printf("read=%zu text=%s eof=%d bufsiz=%d refs=%zu\n", got, &buffer[0], EOF, BUFSIZ,
           sizeof(refs) / sizeof(refs[0]));
    return 0;
}
