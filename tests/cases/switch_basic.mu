// switch_basic.mu — switch: multiple values, half-open ranges, exhaustive enums, else, nesting and break binding
unit switch_basic;

extern int putchar(int c);

enum Color { Red, Green, Blue }

void pi(int v) {
    if v == 0 {
        putchar('0');
        putchar('\n');
        return;
    }
    if v < 0 {
        putchar('-');
        v = -v;
    }
    int[20] d;
    int i = 0;
    while v > 0 {
        d[i] = v % 10;
        v /= 10;
        i += 1;
    }
    while i > 0 {
        i -= 1;
        putchar('0' + d[i]);
    }
    putchar('\n');
}

int classify(int code) {
    switch code {
        0: { return 0; }
        1, 2: { return 1; }
        3..10: { return 2; }
        else: { return -1; }
    }
}

// Exhaustive enum: without an else every member must be covered (a missing one is a compile error)
int paint(Color c) {
    switch c {
        Red:   { return 11; }
        Green: { return 22; }
        Blue:  { return 33; }
    }
}

int main() {
    for i in 0..12 {
        pi(classify(i));
    }
    pi(paint(Red));
    pi(paint(Green));
    pi(paint(Blue));

    // nesting: break/continue inside a switch belong to the enclosing loop
    int n = 0;
    for i in 0..5 {
        switch i {
            0: { continue; }
            1: { n += 100; }
            2..4: { n += 10; }
            else: { n += 1; }
        }
        n += 1;
    }
    pi(n);

    // char range (half-open: 'a'..'{' includes 'z')
    byte ch = 'q';
    switch ch {
        'a'..'{': { pi(1); }
        else: { pi(2); }
    }

    // empty switch body / integer switch without else: a non-match is skipped
    int k = 7;
    switch k { }
    switch k {
        3: { pi(9); }
    }

    // the matched value is evaluated only once
    return 0;
}
