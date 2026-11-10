// cstdio.mu — `libs/std/cstdio.mu` 的端到端验证（文件读写 + 格式化输出 + 常量）
unit cstdio;

use std.cstdio;

int main() {
    std.cstdio.printf("cstdio ok\n");

    // ---- 写文件 ----
    void* f = std.cstdio.fopen("build/cstdio_case.txt", "w");
    if f == null {
        std.cstdio.printf("open for write failed\n");
        return 1;
    }
    std.cstdio.fprintf(f, "line %d\n", 42);
    std.cstdio.fclose(f);

    // ---- 读回来 ----
    f = std.cstdio.fopen("build/cstdio_case.txt", "r");
    if f == null {
        std.cstdio.printf("open for read failed\n");
        return 1;
    }
    char[64] buf;
    std.cstdio.fgets(&buf[0], 64, f);
    std.cstdio.printf("read: %s", &buf[0]);
    std.cstdio.fclose(f);

    // ---- 常量 ----
    std.cstdio.printf("EOF=%d SEEK_END=%d BUFSIZ=%d\n",
                      std.cstdio.EOF, std.cstdio.SEEK_END, std.cstdio.BUFSIZ);

    // ---- 其它输出函数 ----
    int n = std.cstdio.puts("puts works");
    std.cstdio.printf("puts returned %d\n", n);
    std.cstdio.putchar('X');
    std.cstdio.putchar('\n');
    return 0;
}
