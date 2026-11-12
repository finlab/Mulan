unit rr8;

struct Vec {
    usize n;
    int* data;

    usize length() { return n; }
}

ref const int operator[](ref const Vec v, usize i) { return v.data[i]; }

int main() {
    int[2] a = {1, 2};
    Vec v = {2, &a[0]};
    for ref x in v {
        x = 0;
    }
    return 0;
}
