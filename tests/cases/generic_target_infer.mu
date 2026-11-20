// generic_target_infer — the expected type of the call fills in a type parameter that no argument
// determines: initialization, assignment, a return statement and a call argument all provide it.
unit generic_target_infer;

extern int puts(const char* s);

struct Box<T> { T v; }

Box<T> make<T>() {
    Box<T> b = {};
    return b;
}

Box<T> pass<T>(Box<T> b) { return b; }

Box<int> make_int() { return make(); }

int main() {
    Box<int> a = make();
    a.v = 7;
    Box<int> b;
    b = make();
    Box<int> c = pass(a);
    Box<int> d = make_int();
    if c.v != 7 { puts("init/argument"); return 1; }
    if b.v != 0 { puts("assignment"); return 2; }
    if d.v != 0 { puts("return"); return 3; }
    puts("generic target infer ok");
    return 0;
}
