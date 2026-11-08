unit floatlit;
extern int puts(readonly char* s);
int main() {
    float f = 1.5;        // 字面量按上下文适配为 float
    double d = 2.5;
    if f == 1.5 { puts("float literal ok"); }
    d = 3.5f;             // float -> double 加宽
    if d > 3.0 { puts("widen ok"); }
    return 0;
}
