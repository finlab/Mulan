// std.cursor — build a cursor from two closures, without declaring a struct.
//
// The language's cursor protocol is `bool next()` + `T current()` (plus `iterator()` for for-in).
// This wrapper packages the same protocol over two function values, so an ad-hoc source is:
//
//   int i = 0;
//   auto c = cursor(
//       [ref i]() => { if i >= 3 { return false; } i += 1; return true; },
//       [ref i]() => i
//   );
//   for x in c { ... }
//
// The two closures must share their state: capture by reference (`[ref x]`, small scalars/plain
// structs) or capture a class handle (`[x]`, the object is shared and retained). A `[ref x]`
// capture stores the variable's address, so the variable must outlive the cursor: use it for a
// cursor that stays in the same function. To return a cursor from a function, capture a class
// handle instead (the delegate environment retains the object). The compiler rejects returning the
// lambda itself, but it does not see through a struct that contains it, so the `[ref]` rule is the
// caller's responsibility (tracked as a compiler gap).
//
// The element type is inferred from the `current` closure's return type. `Cursor` is a struct and
// satisfies the for-in cursor contract structurally (a generic struct cannot declare an interface
// yet). The calls are through the two-word delegate values: one indirect call per `next`/`current`,
// which is the price of not declaring a monomorphized struct.

public unit std.cursor;

public struct Cursor<T> {
private:
    delegate () => bool nf;
    delegate () => T cf;

public:
    bool next() {
        return this.nf();
    }

    T current() {
        return this.cf();
    }

    Cursor<T> iterator() {
        return this;
    }
}

public Cursor<T> cursor<T>(delegate () => bool nextf, delegate () => T curf) {
    Cursor<T> out = { nf = nextf, cf = curf };
    return out;
}
