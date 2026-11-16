unit is_bind_compose;
// SPEC 10.1.1: a binding `is` must be the whole condition; it does not reach the other operand of
// `and`/`or`. The diagnostic must name the rule instead of reporting "undefined identifier v".
type Option<T> = Some(T) | None;

int main() {
    bool flag = true;
    Option<int> o = Some(5);
    if o is Some(v) and v > 0 {
        return 0;
    }
    return 0;
}
