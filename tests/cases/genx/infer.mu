// Generic inference across units: the template's return type is matched against a client-side target,
// a deferred argument is typed on the client side, and a partial explicit list is completed by the
// target.
public unit genx.infer;

use std.option;

public struct Box<T> {
    public T v;
}

public Box<T> make<T>() {
    Box<T> b = {};
    return b;
}

public struct Pair<A, B> {
    public A a;
    public B b;
}

public Pair<T, U> make_pair<T, U>() {
    Pair<T, U> p = {};
    return p;
}

public T first_like<T>(Option<T> o, T fallback) {
    return match move o { Some(v): v; None: fallback; };
}

public T id<T>(T x) {
    return x;
}
