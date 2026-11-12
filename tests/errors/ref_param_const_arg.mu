unit rp4;

struct P {
    int x;
}

void f(ref P p) {
    p.x = 3;
}

int main() {
    const P a = {1};
    f(a);
    return 0;
}
