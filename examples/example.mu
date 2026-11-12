// example.mu — Mulan phase-1 syntax examples (draft)
// Design goals: small, modern C-like language; value semantics; manual memory management.
// This file uses "settled/suggested" features from spec draft v0.1; it is not guaranteed to compile.

unit example;

use math;

// ---------- C ABI interop ----------
// Functions declared extern are provided by the C runtime; const only affects language-level write checks, not the ABI.
extern int puts(const char* s);
extern int putchar(int c);

// ---------- Value-type structs ----------
// Fields may share a type across several names; assignment copies bytes.
struct Point {
    int x, y;
}

// unit-level variables default to private; uninitialized ones are zero-initialized.
int calls;

// ---------- Functions ----------
// Return type is mandatory; no return value must be written as void; empty parameter list is ().
// Calls before the definition are allowed within the same unit.
int frac(int n) {
    if n <= 1 {
        return 1;
    }
    return frac(n - 1) * n;
}

// for-in iterates the half-open range 0..n; replaces the C-style for.
int sum_to(int n) {
    int sum = 0;
    for i in 0..n {
        sum += i;
    }
    return sum;
}

// while condition must be bool; no integer-to-truth conversion.
int count_down(int n) {
    int count = 0;
    while n > 0 {
        n -= 1;
        count += 1;
    }
    return count;
}

// const int*: no writes through p; interface contract only, no optimization promise.
int sum_ptr(const int* p, usize n) {
    int s = 0;
    for i in 0..n {
        s += p[i];
    }
    return s;
}

// string is a builtin immutable UTF-8 view; s[i] yields a byte.
void print_string(string s) {
    for i in 0..s.length() {
        putchar(s[i]);
    }
}

// Example output only: print a decimal integer via putchar.
void print_int(int v) {
    if v == 0 {
        putchar('0');
        return;
    }
    if v < 0 {
        putchar('-');
        v = -v;
    }
    int[20] digits;        // may declare first, assign later; reading an unwritten element is UB
    int i = 0;
    while v > 0 {
        digits[i] = v % 10;
        v /= 10;
        i += 1;
    }
    while i > 0 {
        i -= 1;
        putchar('0' + digits[i]);
    }
}

// ---------- main ----------
int main() {
    // Declaration syntax: type first; one type may declare several variables, all sharing it.
    // int a = 0, b = 1;
    // int* p, q;       // both p and q are int*
    // int[5] x, y;     // both x and y are int[5]
    // Parens in conditions and loop heads are optional; the spec omits them. C-style if (cond) is still legal.
    const string greeting = "hello, Mulan";
    puts(greeting.cstr());

    calls += 1;                // unit-level variable: starts at 0 after zero-init
    print_int(calls); putchar('\n');

    // const: immutable binding; every variable in one declaration may carry an initializer.
    const int a = frac(10), b = sum_to(100), c = count_down(5);
    print_int(a); putchar('\n');
    print_int(b); putchar('\n');
    print_int(c); putchar('\n');

    // struct is a value type: assignment copies.
    // Aggregate init in a declaration uses = {...}; expression construction uses Point(...).
    // Point p = Point(3, 4) is also legal; the RHS prvalue constructs straight into the target, no extra copy.
    Point p = {3, 4};
    Point q = p;               // copy
    q.x = 10;                  // does not affect p
    if p.x == 3 and q.x == 10 {
        puts("struct is a value type");
    }

    Point r = {x = 1, y = 2};          // named aggregate init
    Point s = {x = 5, y = 6};
    Point t = {7, 8}, u = Point(9, 10); // multiple declarators: braces / expression
    if r.x == 1 and s.y == 6 and t.x == 7 and u.y == 10 {
        puts("struct init ok");
    }

    // Zero init: aggregate types (array/struct/slice/string) use {}; scalars use their own constant (0 / null etc.).
    Point z = {};
    int zeroed = 0;
    int[3] zeroarr = {};
    string empty_text = {};
    if z.x == 0 and z.y == 0 and zeroed == 0 and zeroarr[2] == 0 and empty_text.length() == 0 {
        puts("zero-init ok");
    }

    // Strings: builtin string view; string literals can be passed directly as string args.
    print_string("string view: ");
    print_string("Mulan");
    putchar('\n');

    // Fixed-size char[N] initialized from a string: byte copy + NUL.
    char[20] namebuf = "Mulan";
    puts(&namebuf[0]);

    // Multiple variables share one type: pointers and arrays alike (unlike C declarator rules).
    int x = 1, y = 2;
    int* px = &x, py = &y;                 // both px and py are int*
    int[3] m = {1, 2, 3}, n = {4, 5, 6};   // both m and n are int[3]
    if *px + *py == 3 and m[0] + n[0] == 5 {
        puts("multi-declarator ok");
    }

    // Fixed-length arrays are value types; for-in binds by value by default, ref binding can mutate elements.
    int[5] arr = {1, 2, 3, 4, 5};
    int total = 0;
    for v in arr {
        total += v;
    }
    for ref v in arr {
        v *= 2;
    }
    print_int(total); putchar('\n');

    // Iterate by index: len(arr) returns usize; 0..len(arr) is a half-open range.
    for i in 0..len(arr) {
        print_int(arr[i]); putchar(' ');
    }
    putchar('\n');

    // auto: locals only, must have an initializer; multiple declarators must infer the same type.
    auto inferred_point = Point(11, 12);
    auto inferred_len = len(arr);
    auto inferred_sum = inferred_point.x + inferred_point.y;
    auto i2 = 0, j2 = 1;
    if inferred_len == 5 and inferred_sum == 23 and i2 + j2 == 1 {
        puts("auto inference ok");
    }

    // const view: arr is passed in as a read-only pointer.
    print_int(sum_ptr(&arr[0], len(arr)));
    putchar('\n');

    // break / continue: continue moves to the next iteration, break exits the current loop.
    for i in 0..10 {
        if i == 3 {
            continue;
        }
        if i == 7 {
            break;
        }
        print_int(i); putchar(' ');
    }
    putchar('\n');

    // Manual memory management: alloc/free are builtins backed by the C runtime allocator.
    int* buf = alloc(len(arr) * sizeof(int)) as int*;
    if buf == null {
        return 1;
    }
    for i in 0..len(arr) {
        buf[i] = arr[i];
    }
    int acc = 0;
    for i in 0..len(arr) {
        acc += buf[i];
    }
    free(buf as void*);
    print_int(acc); putchar('\n');

    // Pointers may be null; check before dereferencing.
    int* empty = null;
    if empty == null {
        puts("null pointer check");
    }

    // Slice literal: braces in a declaration create an anonymous array in the current scope and bind a slice view.
    double[] nums = {1.0, 2.0, 3.0, 4.0, 5.0};
    if len(nums) == 5 {
        puts("slice literal ok");
    }

    // Cross-unit calls: after use math; access via math.symbol.
    print_int(math.square(7)); putchar('\n');
    print_int(math.abs(-42)); putchar('\n');

    // while with manual advance; continue in a while jumps to the condition check.
    int k = 0;
    while k < 3 {
        print_int(k); putchar(' ');
        k += 1;
    }
    putchar('\n');

    return 0;
}
