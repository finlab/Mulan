// cstd: the POSIX/glibc additions, which have no counterpart in the MSVC CRT.
//
// The portable subset lives in `cstd_core`; here the subject is exactly what only exists on Linux
// (`aligned_alloc`, `*_r` time functions, `strtok_r`, ...), so the case is fenced with `@platform`.
unit cstd_posix;

use cstd.errno;
use cstd.stdio;
use cstd.stdlib;
use cstd.string;
use cstd.time;
use std.ctypes;

int main() {
    void*[3] stdlib_refs = {aligned_alloc as void*, setenv as void*, unsetenv as void*};
    void*[4] string_refs = {
        strndup as void*, strtok_r as void*, strcasecmp as void*, strncasecmp as void*
    };
    void*[3] time_refs = {localtime_r as void*, gmtime_r as void*, timegm as void*};
    void*[2] io_refs = {fileno as void*, __errno_location as void*};

    int case_ok = strcasecmp("AbC", "abc") == 0 and strncasecmp("xABC", "abc", 3) != 0 ? 1 : 0;

    void* block = aligned_alloc(16, 48);
    if (block == null) {
        return 1;
    }
    int aligned_ok = (block as ulong) % 16 == 0 ? 1 : 0;
    free(block);

    // `Tm` carries platform extensions (glibc adds `tm_gmtoff`/`tm_zone`), so the aggregate is
    // zero-filled first and only the standard fields are assigned.
    Tm epoch = {};
    epoch.tm_mday = 2;
    epoch.tm_mon = 0;
    epoch.tm_year = 70;
    Time gm = timegm(&epoch);
    Tm out = {};
    Tm* back = gmtime_r(&gm, &out);
    int tm_ok = back != null and out.tm_mday == 2 ? 1 : 0;

    setenv("MUC_POSIX_PROBE", "1", 1);
    char* env = getenv("MUC_POSIX_PROBE");
    int env_ok = env != null and *env == '1' ? 1 : 0;
    unsetenv("MUC_POSIX_PROBE");

    char[16] line = {};
    strcpy(&line[0], "a,b,c");
    char* save = null;
    char* first = strtok_r(&line[0], ",", &save);
    char* second = strtok_r(null, ",", &save);
    int tok_ok = first != null and second != null and *second == 'b' ? 1 : 0;

    // `sizeof` is `isize` (SPEC decision 128), so the size_t-typed `c_size` needs an explicit cast;
    // the division keeps the isize spelling and only the `%zu` argument is converted.
    isize total = sizeof(stdlib_refs) + sizeof(string_refs) + sizeof(time_refs) + sizeof(io_refs);
    printf("posix case=%d aligned=%d gm=%lld tm=%d env=%d tok=%d refs=%zu\n",
           case_ok, aligned_ok, gm, tm_ok, env_ok, tok_ok, (total / sizeof(void*)) as usize);
    return 0;
}
