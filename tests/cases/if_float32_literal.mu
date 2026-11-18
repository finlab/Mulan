// `16777217.0f` is a float32 value, so it rounds to 16777216 and the comparison is true — the same
// answer the runtime gives for the same test (#79).
unit if_float32_literal;

extern int puts(const char* s);

#if 16777217.0f == 16777216.0 {
int main() { puts("f32 equal"); return 0; }
}
#else {
int main() { puts("f32 not equal"); return 1; }
}
