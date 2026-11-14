unit sum_extern_param;
type Option<T> = Some(T) | None;

extern void take(Option<int> o);

int main() {
    return 0;
}
