// A generic template is checked when it is instantiated (the cycle can depend on the argument), and a
// by-value cycle between two instances is reported like any other: no wrong layout, no compiler crash.
unit rec_generic_mutual;

struct A<T> { B<T> b; }
struct B<T> { A<T> a; }

int main() {
    A<int> x;
    return 0;
}
