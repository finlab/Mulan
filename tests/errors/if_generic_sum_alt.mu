unit if_generic_sum_alt;
type Pair<T> = Some(T)
#if 1 == 1 {
    | Extra(T)
}
    | None;
int main() { return 0; }
