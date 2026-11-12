unit rp2;

struct P {
    int x;
}

void f(ref P p) {
    P other = {2};
    p = other;
}

int main() {
    P a = {1};
    f(a);
    return 0;
}
