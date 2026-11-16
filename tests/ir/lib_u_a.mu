public unit lib_u_a;

// The unit/function boundary: `lib_u_a` + `b` must not mangle like `lib_u` + `a_b` (#35).
public int b(int x) {
    return x + 1;
}
