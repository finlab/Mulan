// `x is Alt(v)` inside a generic body (issue #8A: IsExpr had no clone case, so the clone was
// null and sema reported "missing expression" with no position).
unit generic_is;
extern int printf(const char* fmt, ...);

type Option<T> = Some(T) | None;

T get_or<T>(Option<T> o, T dflt) {
    if o is Some(v) {
        return v;
    }
    return dflt;
}

bool has_value<T>(Option<T> o) {
    if o is None {
        return false;
    }
    return true;
}

int main() {
    Option<int> some = Some(5);
    Option<int> none = None;
    Option<double> half = Some(1.5);
    printf("%d %d %.1f %d %d\n", get_or<int>(some, -1), get_or<int>(none, -1),
           get_or<double>(half, -1.0), has_value<long>(Some(1)), has_value<long>(None));
    return 0;
}
