unit op_unary_ref_return;

struct V {
    int x;
    ref V operator-() { return this; }
}

int main() { return 0; }
