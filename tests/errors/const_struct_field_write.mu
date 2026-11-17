unit csfw;

const struct P {
    int x;
    P(int a) : x(a) { }
}

int main() {
    P p = P(1);
    p.x = 2;
    return 0;
}
