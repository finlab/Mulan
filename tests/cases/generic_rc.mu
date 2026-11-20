// generic_rc — RC/dtor glue for generic types: owned class fields, a user copy constructor, element
// construction/destruction of generic structs (static and dynamic), and a generic class destructor.
unit generic_rc;

use std.cstdio;

int D = 0;
int C = 0;
int COPY = 0;
int MOVE = 0;

class Tracker {
    public int id;
    Tracker(int i) { id = i; C += 1; }
    ~Tracker() { D += 1; }
}

struct Res<T> {
    T v;
    Res() {}
    ~Res() { D += 1; }
}

struct Holder<T> {
    Res<T> r;
    T v;
    Holder() {}
}

struct W<T> {
    T v;
    W() {}
}

struct M<T> {
    T v;
    M() {}
    M(const ref M<T> o) { v = o.v; COPY += 1; }
}

struct MV<T> {
    T v;
    MV() {}
    MV(move MV<T> o) { v = o.v; MOVE += 1; }
}

class Cc<T> {
    public T v;
    Cc() {}
    ~Cc() { D += 1; }
}

int main() {
    {
        Holder<int> h = {};
        Res<int>[2] arr = {};
    }
    {
        Res<int>[] d = new Res<int>[2];
        d = null;
    }
    {
        W<Tracker> a = {};
        a.v = new Tracker(1);
        {
            W<Tracker> b = a;
            if b.v.id != 1 { printf("inner\n"); return 1; }
        }
        if a.v.id != 1 { printf("outer\n"); return 2; }
    }
    {
        M<int> a = {};
        a.v = 5;
        M<int> b = a;
        if b.v != 5 { printf("copy\n"); return 3; }
    }
    {
        MV<int> a = {};
        a.v = 6;
        MV<int> b = move a;
        if b.v != 6 { printf("move value\n"); return 4; }
    }
    {
        Cc<int> c = new Cc<int>();
    }
    if D != 7 or C != 1 or COPY != 1 or MOVE != 1 {
        printf("counts %d %d %d %d\n", D, C, COPY, MOVE);
        return 5;
    }
    printf("generic rc ok\n");
    return 0;
}
