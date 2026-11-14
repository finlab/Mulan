// A capture-list lambda inside a generic body (issue #8C: `Lambda::captures` was not copied, so
// the re-check rejected the body reference as an implicit capture).
unit generic_capture_lambda;
extern int printf(const char* fmt, ...);

type Get = delegate () => int;

int by_value<T>(T v) {
    Get g = [v] () => (v + 93) as int;
    return g();
}

int by_ref<T>(T start) {
    int acc = start as int;
    Get g = [ref acc] () => acc + 1;
    acc = 10;
    return g();
}

int main() {
    printf("%d %d %d %d\n", by_value<int>(7), by_value<long>(1), by_ref<int>(0), by_ref<long>(0));
    return 0;
}
