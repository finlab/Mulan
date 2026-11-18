// Enum members are compile-time constants (SPEC 3.7.2) and `#if` resolves names the same way (9.2):
// `Color.Red` and the bare `Red` both work, and only the same enum may be compared (#72).
unit if_enum_member;

enum Color { Red, Green = 5, Blue }

#if Color.Red == Red and Green as int == 5 and Blue as int == 6 {
int main() { return 0; }
}
#else {
#error "enum members must be visible in #if"
}
