unit rp5;

class Cat {
    public int age;

    Cat(int a) {
        age = a;
    }
}

void f(ref Cat c) {
    c.age = 3;
}

int main() {
    Cat c = new Cat(1);
    f(c);
    return 0;
}
