// generic_target_more — other target positions: a const binding, a type alias, a target inside a
// generic body, a slice return and a pointer return (with a deferred `null` argument).
unit generic_target_more;

extern int puts(const char* s);

struct Box<T> { T v; Box() {} }
Box<T> make<T>() { Box<T> b = {}; return b; }
Box<T> wrap<T>() { Box<T> b = make(); return b; }

T[] make_arr<T>() { T[] a = {}; return a; }
T* make_ptr<T>(T* p) { return p; }

type BI = Box<int>;

int main() {
    Box<int> a = make();
    a.v = 1;
    const Box<int> cb = make();
    BI bi = make();
    Box<int> w = wrap();
    int[] arr = make_arr();
    int* p = make_ptr(null);
    if a.v != 1 or cb.v != 0 or bi.v != 0 or w.v != 0 { puts("values"); return 1; }
    if arr.length() != 0 { puts("slice"); return 2; }
    if p != null { puts("pointer"); return 3; }
    puts("generic target more ok");
    return 0;
}
