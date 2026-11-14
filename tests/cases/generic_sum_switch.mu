// A sum-type `switch` inside a generic body (issue #8B: the arm's `alt` / `binds` were not
// copied, so a re-check saw a value arm and rejected the pattern).
unit generic_sum_switch;
extern int printf(const char* fmt, ...);

type Option<T> = Some(T) | None;

T get_or<T>(Option<T> o, T dflt) {
    switch o {
        Some(v): { return v; }
        None: { return dflt; }
    }
}

int main() {
    Option<int> some = Some(42);
    Option<int> none = None;
    Option<double> half = Some(2.5);
    printf("%d %d %.2f\n", get_or<int>(some, -1), get_or<int>(none, -1),
           get_or<double>(half, -1.0));
    return 0;
}
