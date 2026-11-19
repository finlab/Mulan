// Two arguments give two different types for `T` (SPEC 3.15.3): the call says which one it is.
unit generic_infer_conflict;

interface IOrder {
    bool operator<(Self a, Self b);
}

T max2<T: IOrder>(T a, T b) { return a < b ? b : a; }

int main() {
    long a = max2(1, 2L);
    return 0;
}
