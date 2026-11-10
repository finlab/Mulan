// dbgvars.mu — 调试信息（-g）的验证素材：标量 / long / float / 结构体 / 指针
unit dbgvars;

extern int printf(const char* fmt, ...);

struct P {
    int x;
    int y;
}

int main(int argc, char** argv) {
    int a = 42;
    long b = 1234567;
    float f = 1.5;
    P p = { x = 7, y = 9 };
    int* pa = &a;
    printf("%d %ld %f %d %d %d\n", a, b, f, p.x, p.y, *pa);
    return 0;
}
