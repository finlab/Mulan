unit enum_value_range;

// An enum is backed by `int`: a literal outside the range cannot be represented (#36).
enum E { A = 2147483648 }

int main() {
    return 0;
}
