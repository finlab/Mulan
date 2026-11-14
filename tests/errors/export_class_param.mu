// `export`: a class value is reference counted, and C cannot participate in that.
unit export_class_param;

class Counter {
    public int n;
    public Counter() { n = 0; }
}

export void bump(Counter c) {
    c.n = c.n + 1;
}

int main() {
    return 0;
}
