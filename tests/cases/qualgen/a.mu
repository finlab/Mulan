public unit qualgen.a;

// Same name and arity as `qualgen.b.pick`, so an unqualified call is ambiguous and a call must be
// qualified with the unit name (SPEC 3.15.3).
public int pick<T>(T x) {
    return 1;
}
