unit cmr;

struct P {
    int x;
    void set(int v) { x = v; }
}

int main() {
    const P p = {1};
    p.set(2);
    return 0;
}
