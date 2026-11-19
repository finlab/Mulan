// option_basic — `std.option.Option<T>`: construction, the `is` presence test, the helpers, a `string`
// payload (the value owns one reference) and `match`.
unit option_basic;

use std.option;
use std.cstdio;
use std.string;

Option<int> half(int x) {
    if x % 2 == 0 { return Some(x / 2); }
    return None;
}

int inc(int x) { return x + 1; }

int main() {
    Option<int> a = Some(3);
    Option<int> b = None;
    printf("%d %d %d %d\n", a is Some, b is Some, std.option.unwrap_or(a, -1),
           std.option.unwrap_or(b, -1));

    // map / and_then keep `None` as `None`
    printf("%d %d\n", std.option.unwrap_or(map(a, inc), 0),
           std.option.unwrap_or(and_then(a, half), 0));
    Option<int> odd = Some(5);
    printf("%d\n", and_then(odd, half) is None);

    // a reference payload is owned by the option and released with it
    {
        Option<string> s = Some(string("hello"));
        match s {
            Some(v): { printf("string %s\n", v.cstr()); }
            None: { printf("string none\n"); }
        }
    }

    // `match` on an Option<int> in expression form
    int k = match a {
        Some(v): v * 10;
        None: 0;
    };
    printf("%d\n", k);
    return 0;
}
