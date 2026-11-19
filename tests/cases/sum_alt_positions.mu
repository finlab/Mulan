// sum_alt_positions — a bare alternative name constructs wherever the target type is known
// (SPEC 10.1.1): declaration, assignment, call argument, `return`, ternary and both operands of
// `==` / `!=`. The argument and comparison positions regressed before (the argument was typed on its
// own and the name was looked up as a function); this case locks all of them together.
unit sum_alt_positions;

use std.cstdio;

variant Option<T> {
    Some T;
    None;
}
variant Num {
    One int;
    Two int;
    None;
}

int take(Option<int> o) {
    if o is Some(v) { return v; }
    return 0;
}

Option<int> make(bool c) {
    if c { return Some(5); }        // `return` position
    return None;
}

int classify(Num n) {
    return n == One(1) ? 1 : (n == Two(2) ? 2 : 0);   // `==` with a payload alternative
}

int widen(Option<int> o) {
    return take(o);
}

int main() {
    // argument position
    if take(Some(3)) != 3 { printf("arg\n"); return 1; }
    if take(None) != 0 { printf("arg none\n"); return 2; }

    // `==` / `!=` with a payload alternative on either side
    Num a = One(1);
    if not (a == One(1)) { printf("eq\n"); return 3; }
    if not (One(1) == a) { printf("eq lhs\n"); return 4; }
    if a == Two(2) { printf("neq\n"); return 5; }
    if not (a != One(2)) { printf("ne\n"); return 6; }

    // value equality still works for the unit alternative next to a payload one
    Num b = None;
    if not (b == None) { printf("none eq\n"); return 7; }

    if classify(a) != 1 { printf("classify one\n"); return 8; }
    Num c = Two(2);
    if classify(c) != 2 { printf("classify two\n"); return 9; }

    // `return` and a nested argument
    if not (make(true) == Some(5)) { printf("make some\n"); return 10; }
    if not (make(false) == None) { printf("make none\n"); return 11; }

    // assignment position
    Option<int> o;
    o = Some(9);
    if o is Some(v) { if v != 9 { printf("assign\n"); return 12; } }

    // method argument position
    if widen(Some(11)) != 11 { printf("widen\n"); return 13; }

    printf("alt positions ok\n");
    return 0;
}
