// generic_bounds.mu — 结构化约束（U34 M3b）：接口运算符成员 + 内置/用户运算符
unit generic_bounds;

extern int puts(const char* s);

interface IAddable {
    Self operator+(Self a, Self b);
}

interface IOrdered {
    bool operator<(Self a, Self b);
}

struct Money {
    long cents;
}

Money operator+(Money a, Money b) {
    Money r = {a.cents + b.cents};
    return r;
}

T sum2<T: IAddable>(T a, T b) { return a + b; }

T max2<T: IOrdered>(T a, T b) { return a < b ? b : a; }

int main() {
    int a = sum2<int>(1, 2);
    int c = max2<int>(3, 7);

    Money m = {10};
    Money n = {32};
    Money r = sum2<Money>(m, n);

    if (a == 3 and c == 7 and r.cents == 42) { puts("generic bounds ok"); }
    return 0;
}
