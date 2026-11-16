// iface_indexable.mu — the shipped `IIndexable<T>` contract actually constrains a container
unit iface_indexable;

extern int puts(const char* s);

// a container satisfied structurally: `length()` + a member subscript
struct Row {
    int[] d;

    isize length() { return d.length(); }
    int operator[](isize i) { return d[i]; }
}

// a generic algorithm over any `IIndexable<int>`: it only uses the contract, never the concrete type
int total<T: IIndexable<int>>(T c) {
    int s = 0;
    for i in 0..c.length() {
        s += c[i];
    }
    return s;
}

int main() {
    Row r = {};
    r.d = new int[4];
    for i in 0..4 {
        r.d[i] = i + 1;
    }
    if total<Row>(r) != 10 { puts("generic subscript failed"); return 1; }

    // a built-in block satisfies it too, through its built-in subscript
    if total<int[]>(r.d) != 10 { puts("slice does not satisfy IIndexable"); return 2; }

    puts("iface indexable ok");
    return 0;
}
