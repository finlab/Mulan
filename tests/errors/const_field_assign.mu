unit cfa;

struct P {
    const int x;
    int y;
    P(int a) : x(a) { y = 0; }
}

int main() {
    P p = P(1);
    p.x = 2;
    return 0;
}
