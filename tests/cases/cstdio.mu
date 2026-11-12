// cstdio.mu — end-to-end verification of `libs/std/cstdio.mu` (file read/write + formatted output + constants)
unit cstdio;

use std.cstdio;

int main() {
    std.cstdio.printf("cstdio ok\n");

    // ---- write the file ----
    void* f = std.cstdio.fopen("build/cstdio_case.txt", "w");
    if f == null {
        std.cstdio.printf("open for write failed\n");
        return 1;
    }
    std.cstdio.fprintf(f, "line %d\n", 42);
    std.cstdio.fclose(f);

    // ---- read it back ----
    f = std.cstdio.fopen("build/cstdio_case.txt", "r");
    if f == null {
        std.cstdio.printf("open for read failed\n");
        return 1;
    }
    char[64] buf;
    std.cstdio.fgets(&buf[0], 64, f);
    std.cstdio.printf("read: %s", &buf[0]);
    std.cstdio.fclose(f);

    // ---- constants ----
    std.cstdio.printf("EOF=%d SEEK_END=%d BUFSIZ=%d\n",
                      std.cstdio.EOF, std.cstdio.SEEK_END, std.cstdio.BUFSIZ);

    // ---- other output functions ----
    int n = std.cstdio.puts("puts works");
    std.cstdio.printf("puts returned %d\n", n);
    std.cstdio.putchar('X');
    std.cstdio.putchar('\n');
    return 0;
}
