unit rp8;

struct P {
    int x;
}

extern void take(ref P p);

int main() {
    P a = {1};
    take(a);
    return 0;
}
