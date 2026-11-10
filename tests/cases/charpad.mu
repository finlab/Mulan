// charpad.mu — char[N] 由 string 初始化：拷贝内容、补 NUL、其余字节清零
unit charpad;

extern int puts(const char* s);

int main() {
    char[8] b = "ab";
    // 字节 2 是 NUL，后面应当被清零
    if b[2] == 0 and b[7] == 0 and len(b) == 8 {
        puts("zero-padded ok");
    }
    puts(&b[0]);
    return 0;
}
