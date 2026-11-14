// unit_init_order.mu — unit-level construction and destruction follow the `use` graph (SPEC 9.1)
//
// The loader returns units in DFS post-order, so init runs c -> b -> a -> entry and cleanup runs the
// exact reverse. Inside a unit the order is the declaration order.
unit unit_init_order;

use initorder.a;

extern int puts(const char* s);

struct MainProbe {
    MainProbe() { puts("init main"); }
    ~MainProbe() { puts("fini main"); }
}

MainProbe probe_main;

int main() {
    puts("main body");
    return 0;
}
