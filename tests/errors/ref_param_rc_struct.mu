unit rp6;

class Cat {
    public int age;

    Cat(int a) {
        age = a;
    }
}

struct Holder {
    Cat c;
    int n;
}

void f(ref Holder h) {
    h.n = 3;
}

int main() {
    Holder h = {new Cat(1), 0};
    f(h);
    return 0;
}
