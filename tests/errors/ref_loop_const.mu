unit rr8;

struct Vec {
    isize n;
    int* data;

    isize length() { return n; }
    ref const int operator[](isize i) { return data[i]; }
}

int main() {
    int[2] a = {1, 2};
    Vec v = {2, &a[0]};
    for ref x in v {
        x = 0;
    }
    return 0;
}
