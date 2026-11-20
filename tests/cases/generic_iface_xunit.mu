unit generic_iface_xunit;

use genx.iface;

extern int puts(const char* s);

// A client struct implementing the library's generic interface.
struct Client<T> : IVal<T> {
    T v;
    Client() {}
    T get() { return v; }
}

int main() {
    Wrap<int> w = {};
    w.v = 4;
    if w.get() != 4 or w.twice() != 2 { puts("library struct"); return 1; }

    Client<int> c = {};
    c.v = 5;
    if c.get() != 5 or c.twice() != 2 { puts("client struct"); return 2; }

    puts("generic iface xunit ok");
    return 0;
}
