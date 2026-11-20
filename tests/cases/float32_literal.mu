// float32_literal — an `f` literal is normalized to f32 once; constants and runtime agree (#247).
unit float32_literal;

extern int puts(const char* s);

const double K = 0.1f;

int main() {
    double a = 0.1f;
    if a != K { puts("different"); return 1; }
    puts("float32 literal ok");
    return 0;
}
