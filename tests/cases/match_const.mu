// `match` on integers, `char` and enums: constant patterns (literals, comma lists, ranges), guards,
// and the expression form. This is the `switch` vocabulary inside `match` (SPEC 10.1.2).
unit match_const;

extern int puts(const char* s);

enum Color { Red, Green, Blue }

int classify(int n) {
    return match n {
        0: 100;
        1, 2: 101;
        3..5: 102;      // half-open: [3, 5)
        5..=7: 103;     // inclusive: [5, 8)
        8 if n < 9: 104;
        else: 105;
    };
}

// An exhaustive enum match needs no `else`, even in the expression form.
int color_code(Color c) {
    return match c {
        Red: 1;
        Green: 2;
        Blue: 3;
    };
}

int char_class(char ch) {
    return match ch {
        'a'..'z': 1;
        '0'..'9': 2;
        else: 0;
    };
}

int main() {
    if classify(0) != 100 { puts("literal"); return 1; }
    if classify(1) != 101 { puts("comma list"); return 2; }
    if classify(2) != 101 { puts("comma list 2"); return 3; }
    if classify(4) != 102 { puts("half-open range"); return 4; }
    if classify(7) != 103 { puts("inclusive range"); return 5; }
    if classify(8) != 104 { puts("guard"); return 6; }
    if classify(9) != 105 { puts("else"); return 7; }

    if color_code(Green) != 2 { puts("enum"); return 8; }
    if char_class('q') != 1 or char_class('7') != 2 or char_class('!') != 0 {
        puts("char ranges");
        return 9;
    }

    // statement form
    int total = 0;
    match 3 {
        1, 2: { total = 10; }
        3:    { total = 30; }
        else: { total = 0; }
    }
    if total != 30 { puts("statement form"); return 10; }

    // a failed guard falls through to the next arm that matches
    int x = 5;
    int r = match x {
        5 if false: 1;
        5: 2;
        else: 3;
    };
    if r != 2 { puts("guard fallthrough"); return 11; }

    // a range arm and a guard on a later arm of the same value space
    int s = 12;
    int t = match s {
        10..13: 1;
        12 if s == 12: 2;
        else: 3;
    };
    if t != 1 { puts("range precedence"); return 12; }

    puts("match const ok");
    return 0;
}
