// Or-patterns: `A(x) | B(x)` shares one arm when every alternative binds the same names, with the
// same modes and types. Alternatives without a payload use the comma list (SPEC 10.1.2).
unit match_or;

extern int puts(const char* s);

variant Value {
    Int int;
    Long int;
    Text const char*;
    None;
    Empty;
}

int payload(Value v) {
    return match v {
        Int(n) | Long(n): n;
        Text(s): 1;
        None: 0;
        Empty: -1;
    };
}

// A failed guard falls through to the next arm that covers the alternative that matched.
int pick(Value v) {
    return match v {
        Int(n) | Long(n) if n > 10: 99;
        Int(n) | Long(n): n;
        else: 0;
    };
}

int unit_kind(Value v) {
    int k = 0;
    match v {
        None, Empty: { k = 7; }
        else: { k = 9; }
    }
    return k;
}

int main() {
    if payload(Int(5)) != 5 { puts("int"); return 1; }
    if payload(Long(7)) != 7 { puts("long"); return 2; }
    if payload(None) != 0 { puts("none"); return 3; }
    if payload(Empty) != -1 { puts("empty"); return 4; }

    if pick(Int(12)) != 99 { puts("guard hit"); return 5; }
    if pick(Long(12)) != 99 { puts("guard hit long"); return 6; }
    if pick(Int(3)) != 3 { puts("guard fallthrough"); return 7; }
    if pick(Long(3)) != 3 { puts("guard fallthrough long"); return 8; }
    if pick(None) != 0 { puts("guard else"); return 9; }

    if unit_kind(None) != 7 or unit_kind(Empty) != 7 or unit_kind(Int(1)) != 9 {
        puts("comma list over alternatives");
        return 10;
    }

    puts("match or ok");
    return 0;
}
