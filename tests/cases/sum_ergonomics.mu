// sum_ergonomics — `is` tests, contextual construction and value equality for sums (SPEC 10.1.1).
unit sum_ergonomics;

use std.cstdio;

type Option<T> = Some(T) | None;
type Shape = Circle(int) | Rect(int, int) | Dot;

class Animal {
    public int legs;
    Animal(int n) { legs = n; }
}

class Cat : Animal {
    Cat() : Animal(4) { }
    public int purr() { return 7; }
}

int unwrap_or(Option<int> o, int fallback) {
    // `is` with a binding: the payload is only visible inside the branch
    if o is Some(v) { return v; }
    return fallback;
}

int main() {
    // contextual construction: the alternative name alone, when the target type is a sum
    Option<int> a = Some(3);
    Option<int> b = None;
    if unwrap_or(a, -1) != 3 { printf("some\n"); return 1; }
    if unwrap_or(b, -1) != -1 { printf("none\n"); return 2; }

    // `is` without a binding, and its negation
    if not (a is Some) { printf("is some\n"); return 3; }
    if b is Some { printf("b is some\n"); return 4; }

    // exhaustive switch still works next to `is`
    int r = 0;
    switch a {
        Some(v): { r = v * 2; }
        None: { r = -1; }
    }
    if r != 6 { printf("switch\n"); return 5; }

    // equality: same alternative and equal payload fields
    Option<int> c = Some(3);
    Option<int> d = Some(4);
    if not (a == c) { printf("eq\n"); return 6; }
    if a == d { printf("neq\n"); return 7; }
    if a == b { printf("some == none\n"); return 8; }
    if not (b == None) { printf("none eq\n"); return 9; }
    if not (a != d) { printf("ne\n"); return 10; }

    // multi-field alternatives compare field by field
    Shape p = Rect(1, 2);
    Shape q = Rect(1, 2);
    Shape t = Rect(2, 1);
    Shape dot = Dot;
    if not (p == q) { printf("rect eq\n"); return 11; }
    if p == t { printf("rect fields\n"); return 12; }
    if p == dot { printf("rect vs dot\n"); return 13; }
    if not (dot == Dot) { printf("dot eq\n"); return 14; }

    // `is` on a class value: a subtype test with an optional binding
    Animal an = new Cat();
    int score = 0;
    if an is Cat kitty { score = kitty.legs + kitty.purr(); }
    if an is Animal beast { score = score + 100; }
    if score != 111 { printf("class is %d\n", score); return 15; }

    printf("sum ergonomics ok\n");
    return 0;
}
