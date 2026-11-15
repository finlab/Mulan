// 100 nested parentheses still compile and evaluate: far above hand-written code and just below the
// cap (320 guard levels ≈ 105 parentheses), so this locks the guard against becoming too strict.
unit deep_parens;
extern int printf(const char* fmt, ...);
int main() {
    int x = ((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((6 * 7))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))));
    printf("%d\n", x);
    return 0;
}
