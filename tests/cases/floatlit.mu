unit floatlit;
extern int puts(const char* s);
int main() {
    float f = 1.5;        // literal adapts to float by context
    double d = 2.5;
    if f == 1.5 { puts("float literal ok"); }
    d = 3.5f;             // float -> double widening
    if d > 3.0 { puts("widen ok"); }
    return 0;
}
