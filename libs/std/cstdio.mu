// cstdio.mu —— C 标准库 <stdio.h> 的绑定（ABI 对齐 msvcrt / UCRT）
//
// 只做声明，不实现：符号由链接器从 C 运行时解析。
// `FILE` 是 C 侧的不透明结构，这里统一用 `void*` 表示；`...` 为 C 可变参数
// （实参按 C 默认提升规则传递，`string` 会自动转成 `const char*`）。
//
// 未绑定：`vprintf` 族（需要 `va_list`）、`fgetpos`/`fsetpos`（需要 `fpos_t`）、
// `getline`（POSIX 扩展）、`scanf` 的 `%n` 语义。

public unit std.cstdio;

// ---------------------------------------------------------------- 常量
public const int EOF = -1;
public const int BUFSIZ = 512;
public const int FILENAME_MAX = 260;
public const int SEEK_SET = 0;
public const int SEEK_CUR = 1;
public const int SEEK_END = 2;

// ---------------------------------------------------------------- 文件操作
extern void* fopen(const char* path, const char* mode);
extern void* freopen(const char* path, const char* mode, void* stream);
extern int fclose(void* stream);
extern int fflush(void* stream);
extern int remove(const char* path);
extern int rename(const char* old_path, const char* new_path);
extern void* tmpfile();

// ---------------------------------------------------------------- 格式化输出
extern int printf(const char* fmt, ...);
extern int fprintf(void* stream, const char* fmt, ...);
extern int sprintf(char* buf, const char* fmt, ...);
extern int snprintf(char* buf, usize size, const char* fmt, ...);

// ---------------------------------------------------------------- 字符 / 行
extern int fgetc(void* stream);
extern int getchar();
extern char* fgets(char* buf, int size, void* stream);
extern int fputc(int c, void* stream);
extern int putchar(int c);
extern int fputs(const char* s, void* stream);
extern int puts(const char* s);
extern int ungetc(int c, void* stream);

// ---------------------------------------------------------------- 格式化输入
extern int scanf(const char* fmt, ...);
extern int fscanf(void* stream, const char* fmt, ...);
extern int sscanf(const char* s, const char* fmt, ...);

// ---------------------------------------------------------------- 定位
extern int fseek(void* stream, long offset, int origin);
extern long ftell(void* stream);
extern void rewind(void* stream);

// ---------------------------------------------------------------- 状态与错误
extern void clearerr(void* stream);
extern int feof(void* stream);
extern int ferror(void* stream);
extern void perror(const char* s);
