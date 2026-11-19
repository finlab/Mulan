// A `match` on a variant inside a generic body: cloning/substitution must carry the arms.
unit generic_sum_switch;
extern int printf(const char* fmt, ...);

variant Option<T> {
    Some T;
    None;
}

T get_or<T>(Option<T> o, T dflt) {
    return match o {
        Some(v): v;
        None: dflt;
    };
}

int main() {
    Option<int> some = Some(42);
    Option<int> none = None;
    Option<double> half = Some(2.5);
    printf("%d %d %.2f\n", get_or<int>(some, -1), get_or<int>(none, -1),
           get_or<double>(half, -1.0));
    return 0;
}
