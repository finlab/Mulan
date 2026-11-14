// std_interfaces — the predeclared capability interfaces in `std.interfaces` (built-in types and
// user types satisfy them structurally; no vtable, no boxing).
unit std_interfaces;

use std.interfaces;
use std.cstdio;

// `T: IAddable` — `+` returns T.
T sum3<T: IAddable>(T a, T b, T c) {
    return a + b + c;
}

// `T: IOrdered` inherits IEquatable, so equality and all four orderings are available.
T biggest<T: IOrdered>(T a, T b) {
    if a < b { return b; }
    return a;
}

bool same<T: IEquatable>(T a, T b) {
    return a == b;
}

// `T: INumber` bundles + - * / and unary minus plus the ordering contract.
T scale_add<T: INumber>(T value, T scale, T delta) {
    return -value * scale + delta;
}

bool above<T: IOrdered>(T a, T b) {
    return a >= b and not (a < b);
}

struct Money : IAddable, IOrdered {
    public int cents;
    public override Money operator+(Money other) {
        Money r = { cents = cents + other.cents };
        return r;
    }
    public override bool operator==(Money other) { return cents == other.cents; }
    public override bool operator!=(Money other) { return cents != other.cents; }
    public override bool operator<(Money other) { return cents < other.cents; }
    public override bool operator<=(Money other) { return cents <= other.cents; }
    public override bool operator>(Money other) { return cents > other.cents; }
    public override bool operator>=(Money other) { return cents >= other.cents; }
}

// The cursor half of the iterable contract is checked at the cursor (`: IIterator<int>`), the
// container half at the container (`: IEnumerable<int>`). A class cursor is returned as an interface
// value (an interface value cannot hold a struct — that would be boxing); `for-in` iterates it
// through the itable.
class Counter : IIterator<int> {
    int i;
    Counter() { i = 0; }
    override bool next() {
        i = i + 1;
        return i <= 3;
    }
    override int current() { return i * 10; }
}

class Counts : IEnumerable<int> {
    Counter c;
    Counts() { c = new Counter(); }
    override IIterator<int> iterator() { return c; }
}

// The same pair in generic form: a generic class implementing a generic interface.
class Cursor<T> : IIterator<T> {
    T v;
    int i;
    Cursor() {
        i = 0;
        v = 0 as T;
    }
    override bool next() {
        i = i + 1;
        v = i as T;
        return i <= 3;
    }
    override T current() { return v; }
}

class Source<T> : IEnumerable<T> {
    Cursor<T> c;
    Source() { c = new Cursor<T>(); }
    override IIterator<T> iterator() { return c; }
}

int main() {
    if sum3<int>(1, 2, 3) != 6 { printf("int add\n"); return 1; }
    if sum3<double>(1.5, 2.0, 0.5) != 4.0 { printf("double add\n"); return 2; }

    if biggest<int>(4, 9) != 9 { printf("int order\n"); return 3; }
    if biggest<double>(2.5, -1.0) != 2.5 { printf("double order\n"); return 4; }

    if not same<int>(3, 3) { printf("int eq\n"); return 5; }
    if same<double>(1.0, 2.0) { printf("double eq\n"); return 6; }

    // -3 * 2 + 10 = 4
    if scale_add<int>(3, 2, 10) != 4 { printf("int number\n"); return 7; }
    // -1.5 * 2.0 + 4.0 = 1.0
    if scale_add<double>(1.5, 2.0, 4.0) != 1.0 { printf("double number\n"); return 8; }

    if not above<int>(5, 5) { printf("order ge\n"); return 9; }

    // A user value type satisfies the same contracts through its operators.
    Money a = { cents = 120 };
    Money b = { cents = 80 };
    if sum3<Money>(a, b, a).cents != 320 { printf("money add\n"); return 10; }
    if biggest<Money>(a, b).cents != 120 { printf("money order\n"); return 11; }
    if not same<Money>(a, a) { printf("money eq\n"); return 12; }

    // The iterable pair: the cursor declares IIterator<int>, the container IEnumerable<int>.
    Counts counts = new Counts();
    int total = 0;
    for v in counts { total = total + v; }
    if total != 60 { printf("iterable\n"); return 13; }

    // Generic form: Source<int> implements IEnumerable<int> through Cursor<int>.
    Source<int> src = new Source<int>();
    int gtotal = 0;
    for v in src { gtotal = gtotal + v; }
    if gtotal != 6 { printf("generic iterable\n"); return 14; }

    printf("std interfaces ok\n");
    return 0;
}
