// iface_generic.mu — instantiated generic interfaces are real interfaces: a class implements
// `IIterator<int>` / `IEnumerable<int>`, a generic class implements `IEnumerable<T>`, a generic
// interface inherits another one, and a struct satisfies a contract without `override`.
unit iface_generic;

use std.interfaces;
use std.cstdio;

// --- a concrete cursor + container pair -------------------------------------------------------
class Counter : IIterator<int> {
    int i;
    Counter(int start) { i = start; }
    override bool next() {
        i = i + 1;
        return i <= 3;
    }
    override int current() { return i * 10; }
}

class Tens : IEnumerable<int> {
    override IIterator<int> iterator() { return new Counter(0); }
}

// --- the same pair in generic form -------------------------------------------------------------
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
    override IIterator<T> iterator() { return new Cursor<T>(); }
}

// --- generic interface inheritance -------------------------------------------------------------
interface Base<T> {
    T first();
}

interface Derived<T> : Base<T> {
    T second();
}

class Pair : Derived<int> {
    int v;
    Pair(int x) { v = x; }
    override int first() { return v; }
    override int second() { return v * 2; }
}

// --- a struct satisfies a contract without `override` ------------------------------------------
struct Point : IAddable, IOrdered {
    public int x;
    public Point operator+(Point other) {
        Point r = { x = x + other.x };
        return r;
    }
    public bool operator==(Point other) { return x == other.x; }
    public bool operator!=(Point other) { return x != other.x; }
    public bool operator<(Point other) { return x < other.x; }
    public bool operator<=(Point other) { return x <= other.x; }
    public bool operator>(Point other) { return x > other.x; }
    public bool operator>=(Point other) { return x >= other.x; }
}

// --- a struct can also implement IEnumerable<T> (the cursor is a class) ------------------------
struct StructTens : IEnumerable<int> {
    Counter c;
    IIterator<int> iterator() { return c; }
}

// `IEnumerable<int>` as a constraint: satisfied structurally by the iterator() signature.
int peek<T: IEnumerable<int>>(T seq) {
    IIterator<int> it = seq.iterator();
    bool any = it.next();
    return it.current();
}

int main() {
    // concrete container: for-in over an interface cursor
    Tens t = new Tens();
    int sum = 0;
    for v in t { sum = sum + v; }
    if sum != 60 { printf("tens %d\n", sum); return 1; }

    // the interface value itself: size and dispatch
    IIterator<int> it = t.iterator();
    int size = sizeof(it) as int;
    if size != 16 { printf("sizeof %d\n", size); return 2; }

    // generic container
    Source<int> s = new Source<int>();
    int g = 0;
    for v in s { g = g + v; }
    if g != 6 { printf("source %d\n", g); return 3; }

    // generic interface inheritance, dispatch through the derived interface value
    Pair p = new Pair(7);
    Derived<int> d = p;
    if d.first() + d.second() != 21 { printf("derived\n"); return 4; }

    // downcast back to the class
    Pair back = d as Pair;
    if back.first() != 7 { printf("downcast\n"); return 5; }

    // struct contract without `override`, and the same contract used as a generic bound
    Point a = { x = 2 };
    Point b = { x = 3 };
    if (a + b).x != 5 { printf("struct operator\n"); return 6; }
    if not (a < b) { printf("struct order\n"); return 7; }

    StructTens st;
    st.c = new Counter(0);
    int ssum = 0;
    for v in st { ssum = ssum + v; }
    if ssum != 60 { printf("struct container %d\n", ssum); return 8; }

    if peek<Tens>(t) != 10 { printf("bound\n"); return 9; }

    printf("generic interface ok\n");
    return 0;
}
