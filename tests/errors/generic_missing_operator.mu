unit generic_missing_operator;

struct P {
    public int x;
}

T addup<T>(T a, T b) { return a + b; }

int main() {
    P p = P(1);
    P q = P(2);
    P r = addup<P>(p, q);
    return 0;
}
