// generic_implicit_ctor — a generic instance keeps its implicit constructor (#161).
unit generic_implicit_ctor;

extern int puts(const char* s);

struct Wrap<T> {
    T v;
    implicit Wrap(T x) { v = x; }
}

int main() {
    Wrap<int> w = 5;
    if w.v != 5 { puts("value"); return 1; }
    puts("generic implicit ctor ok");
    return 0;
}
