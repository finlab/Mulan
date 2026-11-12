unit rp3;

struct P {
    int x;
}

void f(ref P p) {
    p.x = 3;
}

int main() {
    f(P(1));
    return 0;
}
