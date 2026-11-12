unit pe2;

class Cat {
public:
    int age;
    Cat() { age = 1; }
}

void drop(const Cat c) {
    c = null;
}

int main() {
    Cat c = new Cat();
    drop(c);
    return 0;
}
