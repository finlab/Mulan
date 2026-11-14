// delegate_capture — capturing closures (U41): `[x]` copies the value into the environment object,
// `[ref x]` stores its address so writes reach the captured variable. A capturing lambda's value is a
// `delegate`; a non-capturing one is still a plain `fn`.
unit delegate_capture;

use std.cstdio;
use std.string;
use std.interfaces;

type Get = delegate () => int;
type Acc = delegate (int) => void;

Get make(int v) {
    // by value: the environment is heap allocated, so the delegate outlives this call
    return [v] () => v * 2;
}

int main() {
    // ---- by value: the lambda sees its own copy ----
    int x = 7;
    Get g = [x] () => x;
    x = 99;
    if g() != 7 { printf("copy\n"); return 1; }

    // ---- by value of an RC value: the environment takes its own reference ----
    String s = "hello";
    Get slen = [s] () => s.length() as int;
    s = "";
    if slen() != 5 { printf("string capture\n"); return 2; }

    // ---- by reference: writes reach the captured variable ----
    int total = 0;
    Acc add = [ref total] (int v) => { total = total + v; };
    add(1);
    add(4);
    if total != 5 { printf("write back\n"); return 3; }

    // ---- a `ref` capture of a caller's storage: the out-parameter pattern ----
    int out = 0;
    {
        Acc set = [ref out] (int v) => { out = v * 3; };
        set(5);
    }
    if out != 15 { printf("out param\n"); return 4; }

    // ---- escaping by value is safe ----
    Get m = make(21);
    if m() != 42 { printf("escape\n"); return 5; }

    // ---- nested: the inner lambda captures the outer capture ----
    int base = 5;
    Get outer = [base] () => {
        Get inner = [base] () => base + 1;
        return inner();
    };
    if outer() != 6 { printf("nested\n"); return 6; }

    // ---- capture by value of a class reference keeps the object alive ----
    Live a = new Live();
    Get id = [a] () => a.tag();
    a = null;
    if id() != 3 { printf("class capture\n"); return 7; }
    id = null;  // releases the environment, which releases the object
    if LiveCount() != 0 { printf("class release\n"); return 8; }

    // ---- a capturing lambda satisfies `IEnumerable<T>` like any other class cursor ----
    Countdown c = new Countdown(3);
    int sum = 0;
    for v in c { sum = sum + v; }
    if sum != 6 { printf("iterable\n"); return 9; }

    printf("delegate capture ok\n");
    return 0;
}

// ---- helpers (declared after use is fine: the unit is parsed in two passes) -------------------
int live_count = 0;
int LiveCount() { return live_count; }

class Live {
    public int t;
    Live() {
        t = 3;
        live_count = live_count + 1;
    }
    ~Live() { live_count = live_count - 1; }
    int tag() { return t; }
}

class Counter : IIterator<int> {
    int i;
    Counter(int start) { i = start; }
    override bool next() {
        i = i - 1;
        return i >= 0;
    }
    override int current() { return i + 1; }
}

class Countdown : IEnumerable<int> {
    int start;
    Countdown(int n) { start = n; }
    // the cursor is a class, so it can be handed out as an interface value
    override IIterator<int> iterator() { return new Counter(start); }
}
