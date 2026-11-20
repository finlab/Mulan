// generic_partial_infer — explicit leading type arguments, the rest inferred from the arguments.
unit generic_partial_infer;

extern int puts(const char* s);

struct Pair<A, B> { A a; B b; }

B second<A, B>(Pair<A, B> p) { return p.b; }
T pick<T, U, V>(T t, U u, V v) { return t; }
U id<T, U>(U u) { return u; }

int main() {
    Pair<int, int> p = {};
    p.a = 1;
    p.b = 2;
    if second<int>(p) != 2 { puts("second"); return 1; }
    if pick<int>(7, 2, 3) != 7 { puts("pick"); return 2; }
    if id<int>(5) != 5 { puts("id"); return 3; }
    puts("generic partial infer ok");
    return 0;
}
