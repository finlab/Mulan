unit para_this;

class C {
    int v;
    void go() {
        para {
            { v = 1; }
        }
    }
}

int main() {
    C c = new C();
    c.go();
    return 0;
}
