unit enum_value_overflow;

// The implicit successor of the last explicit value overflows `int` as well (#36).
enum E { A = 2147483647, B }

int main() {
    return 0;
}
