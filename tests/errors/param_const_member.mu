unit pe3;

class Cat {
public:
    int age;
    Cat() { age = 1; }
}

void bump(const Cat c) {
    c.age = 2;
}

int main() {
    Cat c = new Cat();
    bump(c);
    return 0;
}
