// for_ref_const.mu — read-only iteration over a const sequence (and over a mutable one)
unit for_ref_const;

extern int puts(const char* s);

int main() {
    int[3] a = {1, 2, 3};
    const int[] rs = a;
    int sum = 0;
    for ref const v in rs { sum += v; }
    if sum != 6 { puts("const slice iteration failed"); return 1; }

    int[] ms = a;
    int sum2 = 0;
    for ref const v in ms { sum2 += v; }
    if sum2 != 6 { puts("mutable slice read-only iteration failed"); return 2; }

    puts("for ref const ok");
    return 0;
}
