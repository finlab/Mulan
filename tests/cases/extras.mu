unit extra2;

extern int puts(const char* s);

enum Color { Red, Green = 5, Blue }

struct Pair { int a, b; }

int sum_slice(const int[] s) {
    int t = 0;
    for v in s { t += v; }
    return t;
}

int main() {
    Color c = Green;
    int cv = c as int;
    if cv == 5 { puts("enum ok"); }

    int[4] arr = {1, 2, 3, 4};
    int[] s = arr;
    if sum_slice(s) == 10 { puts("slice from array ok"); }

    if sizeof(int) == 4 and alignof(int) == 4 { puts("sizeof ok"); }

    int* p = &arr[0];
    p = p + 1;
    if *p == 2 { puts("ptr arith ok"); }

    Pair pr = {1, 2};
    Pair* pp = &pr;
    if (*pp).b == 2 { puts("ptr struct ok"); }

    int x = 0;
    while true {
        x += 1;
        if x == 3 { break; }
    }
    if x == 3 { puts("while true ok"); }
    return 0;
}
