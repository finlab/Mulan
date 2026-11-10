unit lib_imp_ok;

public int add2(int x) {
    return x + 2;
}

public const int K2 = 5;

public enum Color {
    Red,
    Green
}

public struct Pair {
    int a, b;
}

public int same(int x) {
    return x;
}

public int same(double x) {
    return 1;
}
