// cstd: the MSVC/Windows CRT-only surface, which has no counterpart in glibc.
//
// These symbols are what a Windows program may rely on, so they must stay bound and must be exercised
// somewhere; they cannot live in `cstd_core`, whose subject is the portable subset.
unit cstd_windows;

use cstd.errno;
use cstd.stdio;
use cstd.stdlib;
use cstd.string;
use std.ctypes;

int never = 0;

int main() {
    void*[2] io_refs = {__acrt_iob_func as void*, _flushall as void*};
    void*[3] stdlib_refs = {_aligned_malloc as void*, _aligned_free as void*, _putenv as void*};
    void*[7] string_refs = {
        _strdup as void*, _stricmp as void*, _strnicmp as void*, strtok_s as void*,
        strerror_s as void*, _expand as void*, _msize as void*
    };
    void*[1] errno_refs = {_errno as void*};

    char[8] buffer = {};
    if (never != 0) {
        _snprintf(&buffer[0], 8, "%d", 1);
    }

    // `_aligned_malloc` / `_aligned_free` round-trip, and `_stricmp` ignores case.
    void* block = _aligned_malloc(64, 16);
    if (block == null) {
        return 1;
    }
    _aligned_free(block);
    int case_ok = _stricmp("AbC", "abc") == 0 ? 1 : 0;

    c_size total = sizeof(io_refs) + sizeof(stdlib_refs) + sizeof(string_refs) + sizeof(errno_refs);
    printf("windows-only ok=%d refs=%zu\n", case_ok, total / sizeof(void*));
    return 0;
}
