// derive_compare.mu — `!=` comes from `==`, and `> <= >=` come from `<` (a declared operator wins)
unit derive_compare;

extern int puts(const char* s);

struct N {
    int v;

    bool operator==(N o) { return v == o.v; }
    bool operator<(N o) { return v < o.v; }
}

// `<` is never true here, so the derived operators must all behave as "equal or greater".
struct NeverLess {
    int v;

    bool operator==(NeverLess o) { return true; }
    bool operator<(NeverLess o) { return false; }
}

// A declared operator wins over derivation: this `>` is the reverse of what `<` would derive.
struct Reverse {
    int v;

    bool operator==(Reverse o) { return v == o.v; }
    bool operator<(Reverse o) { return v < o.v; }
    bool operator>(Reverse o) { return v > o.v; }
}

int main() {
    N one = {1};
    N two = {2};
    if not (one < two) { puts("<"); return 1; }
    if one > two { puts(">"); return 2; }
    if not (one <= two) { puts("<="); return 3; }
    if one >= two { puts(">="); return 4; }
    if not (one != two) { puts("!="); return 5; }

    // equal values: only the inclusive forms hold
    N one2 = {1};
    if one < one2 or one2 < one { puts("< equal"); return 6; }
    if one > one2 or one2 > one { puts("> equal"); return 7; }
    if not (one <= one2) or not (one >= one2) { puts("inclusive equal"); return 8; }
    if one != one2 { puts("!= equal"); return 9; }

    NeverLess a = {1};
    NeverLess b = {9};
    if a < b or b < a { puts("nl <"); return 10; }
    if a > b or b > a { puts("nl >"); return 11; }
    if not (a <= b) or not (b <= a) { puts("nl <="); return 12; }
    if not (a >= b) or not (b >= a) { puts("nl >="); return 13; }

    Reverse r1 = {1};
    Reverse r2 = {2};
    if r1 > r2 or not (r2 > r1) { puts("declared > not used"); return 14; }

    puts("derive compare ok");
    return 0;
}
