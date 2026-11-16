unit std_iface_missing;
use std.interfaces;
struct Plain { int v; }
T add_all<T: IAdd>(T a, T b) { return a + b; }
int main() {
    Plain p;
    Plain q;
    return add_all<Plain>(p, q).v;
}
