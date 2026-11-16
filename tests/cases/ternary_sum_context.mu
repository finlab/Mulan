unit ternary_sum_context;
use std.cstdio;

// A target-typed expression propagates its expected type to the arms: the bare alternative of a
// sum resolves there (`Option<int> v = c ? Some(7) : None;`), SPEC 10.1.1.
type Option<T> = Some(T) | None;

int main() {
    bool yes = true;
    Option<int> a = yes ? Some(7) : None;
    Option<int> b = yes ? None : Some(9);
    int sum = 0;
    if a is Some(v) {
        sum += v;
    } else {
        sum += 100;
    }
    if b is Some(w) {
        sum += w;
    } else {
        sum += 1;
    }
    printf("sum=%d\n", sum);
    return 0;
}
