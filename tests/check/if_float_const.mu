// Floating-point literals and `as` conversions are compile-time constants (SPEC 3.7.2), so a
// `const double` participates in `#if` (#74).
unit if_float_const;

const double VALUE = 1.5;
const float SMALL = 0.25f;

#if VALUE > 1.0 and SMALL < VALUE {
int main() { return 0; }
}
#else {
#error "float constants must be visible in #if"
}
