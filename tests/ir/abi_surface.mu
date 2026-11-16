// The C ABI surface of `export`ed functions: an aggregate the C ABI passes by reference must be
// declared with `byval`, and one it returns by hidden pointer with `sret`. Checked on the IR so the
// shape is visible even where no C compiler is involved (tests/abi covers the runtime behaviour).
unit abi_surface;

public struct Pair {
    public long a;
    public long b;
}

public struct Wide {
    public long a;
    public long b;
    public long c;
}

public struct Small {
    public int a;
    public int b;
}

export long take_pair(Pair p) { return p.a + p.b; }

export Pair make_pair(long a, long b) {
    Pair r = { a = a, b = b };
    return r;
}

export long take_wide(Wide w) { return w.a + w.b + w.c; }

export Small flip(Small s) {
    Small r = { a = s.b, b = s.a };
    return r;
}

export long take_small(Small s) { return s.a * 10 + s.b; }

int main() {
    return 0;
}

