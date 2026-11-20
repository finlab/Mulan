// generic_issue_regressions — clone paths that used to lose information: `for const ref` in a generic
// body (#146), `T()` zero construction (#143), and a struct's `ref` return satisfying a declarative
// interface (#166).
unit generic_issue_regressions;

extern int puts(const char* s);

T zero<T>() { return T(); }

int sum<T>(T[] a) {
    int n = 0;
    for const ref x in a { n = n + x as int; }
    return n;
}

interface I {
    ref int get();
}

struct S : I {
    int n;
    S() { n = 0; }
    ref int get() { return n; }
}

int main() {
    int[] a = { 1, 2, 3 };
    if sum(a) != 6 { puts("for const ref"); return 1; }
    if zero<int>() != 0 { puts("zero"); return 2; }
    S s = {};
    s.get() = 4;
    if s.n != 4 { puts("ref return"); return 3; }
    puts("generic issue regressions ok");
    return 0;
}
