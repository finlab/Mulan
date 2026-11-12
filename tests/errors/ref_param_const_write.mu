unit rp1;

struct P {
    int x;
}

void f(ref const P p) {
    p.x = 3;
}

int main() {
    P a = {1};
    f(a);
    return 0;
}
