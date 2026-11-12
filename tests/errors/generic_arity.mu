unit generic_arity;

T id<T>(T x) { return x; }

int main() {
    int n = id<int, long>(1);
    return 0;
}
