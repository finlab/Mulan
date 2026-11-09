unit struct_methods;
extern int puts(readonly char* s);
struct Counter {
    int v;
    void inc() { v += 1; }
    void add(int x) { v += x; }
    int get() { return v; }
    int twice() { return get() * 2; }
    int pick(int x) { return x + v; }
    int pick(long x) { return (x as int) + v + 1; }
}
struct Box { Counter c; }
int main() {
    Counter c = {0};
    c.inc();
    c.add(3);
    if c.get() != 4 { puts("get failed"); return 1; }
    if c.twice() != 8 { puts("implicit call failed"); return 1; }
    if c.pick(1) != 5 { puts("overload int failed"); return 1; }
    long big = 1;
    if c.pick(big) != 6 { puts("overload long failed"); return 1; }
    Counter[2] arr = 0;
    arr[1].add(7);
    if arr[1].get() != 7 { puts("array receiver failed"); return 1; }
    Box b = 0;
    b.c.add(9);
    if b.c.get() != 9 { puts("field receiver failed"); return 1; }
    puts("struct methods ok");
    return 0;
}
