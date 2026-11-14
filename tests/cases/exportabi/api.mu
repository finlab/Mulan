public unit exportabi.api;

// An `export`ed function and the unit's internal functions call each other freely: the C ABI is a
// property of the *symbol* (the callee's declaration), and every call site is emitted from it, so
// an internal call into `take`/`make` below uses the C ABI while `take` itself calls `helper` with
// the internal convention.

public struct Pair {
    public long a;
    public long b;
}

// internal: aggregates travel in registers, no copy
long helper(Pair p) { return p.a * 10 + p.b; }

// exported: C ABI (byval/sret), body is ordinary Mu code
export long take(Pair p) { return helper(p) + 1; }

export Pair make(long a, long b) {
    Pair r = { a = a, b = b };
    return r;
}

// internal call sites of the exported symbols (one symbol, one convention)
public long via_export(Pair p) { return take(p) * 2; }

public Pair via_make(long a, long b) {
    Pair r = make(a, b);
    r.b = r.b + 1;
    return r;
}
