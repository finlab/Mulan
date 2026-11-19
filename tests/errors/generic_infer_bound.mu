// The inferred type argument breaks the template's constraint (SPEC 3.15.3): the diagnostic says the
// type was inferred and that an explicit list chooses another one.
unit generic_infer_bound;

interface IOrder {
    bool operator<(Self a, Self b);
}

struct Point {
    int x;
}

T max2<T: IOrder>(T a, T b) { return a < b ? b : a; }

int main() {
    Point p = {1};
    Point q = {2};
    Point r = max2(p, q);
    return r.x;
}
