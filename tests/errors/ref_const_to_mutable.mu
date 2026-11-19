unit rr9;

struct Vec {
    usize n;
    int* data;
}

const ref int peek(const ref Vec v, usize i) { return v.data[i]; }

void bump(ref int x) { x += 1; }

int main() {
    int[2] a = {1, 2};
    Vec v = {2, &a[0]};
    bump(peek(v, 0));
    return 0;
}
