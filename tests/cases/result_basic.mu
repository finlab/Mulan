// result_basic — `std.result.Result<T,E>`: an error payload instead of `None`, the helpers, and the
// `Option` bridge written with `match`. Presence is the language's `is` test, not a helper.
unit result_basic;

use std.result;
use std.option;
use std.cstdio;

Result<int, int> divide(int a, int b) {
    if b == 0 { return Err(-1); }
    return Ok(a / b);
}

int double_it(int x) { return x * 2; }

int main() {
    Result<int, int> good = divide(10, 2);
    Result<int, int> bad = divide(10, 0);

    printf("%d %d %d %d\n", good is Ok, bad is Err, std.result.unwrap_or(good, 0),
           std.result.unwrap_or(bad, 0));

    // the error value is data, not just a flag
    int why = match bad {
        Ok(v): v;
        Err(e): e;
    };
    printf("%d\n", why);

    // bridging to `Option` is a `match` (keeps the helper set small and the conversion explicit)
    Option<int> opt = match good {
        Ok(v): Some(v);
        Err(e): None;
    };
    printf("%d\n", std.option.unwrap_or(opt, -9));

    // map_ok transforms the success, map_err the failure
    printf("%d %d\n", std.result.unwrap_or(map_ok(good, double_it), 0),
           std.result.unwrap_or(map_err(bad, double_it), 0));

    // expect_ok throws when the result failed
    printf("%d\n", expect_ok(good, "should not throw"));
    return 0;
}
