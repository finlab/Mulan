// struct_iface.mu — `struct S : I` 静态满足（U33 S1/S2）：只列接口、显式 override
unit struct_iface;

extern int puts(const char* s);

interface Animal {
    void speak();
}

interface Named {
    usize name_len();
}

struct Cat : Animal {
    int lives;

    override void speak() { puts("meow"); }
}

struct Tagged : Animal, Named {
    int id;

    override void speak() { puts("tag"); }
    override usize name_len() { return 3; }
}

int main() {
    Cat c = {9};
    c.speak();
    Tagged t = {1};
    t.speak();
    if (c.lives == 9 and t.name_len() == 3) { puts("struct iface ok"); }
    return 0;
}
