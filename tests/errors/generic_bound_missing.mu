unit generic_bound_missing;

interface IAddable {
    Self operator+(Self a, Self b);
}

struct Money {
    long cents;
}

T sum2<T: IAddable>(T a, T b) { return a + b; }

int main() {
    Money m = {1};
    Money n = {2};
    Money r = sum2<Money>(m, n);
    return 0;
}
