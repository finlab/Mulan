// struct_iface_default.mu — a struct inherits an interface default as an ordinary method (U34 M4)
unit struct_iface_default;

extern int puts(const char* s);

interface Animal {
    void speak();
    void greet() { this.speak(); this.speak(); }
    void announce() { this.greet(); puts("announced"); }
}

interface Sized {
    int size();
    int half() { return this.size() / 2; }
}

struct Cat : Animal, Sized {
    int lives;

    override void speak() { puts("meow"); }
    override int size() { return 10; }
}

struct Robot : Animal {
    // an explicit implementation wins over the default: the default is not materialized
    override void greet() { puts("custom greet"); }

    override void speak() { puts("beep"); }
}

int main() {
    Cat c = {9};
    c.greet();
    c.announce();
    if (c.half() == 5) { puts("half ok"); }

    Robot r = {};
    r.greet();
    r.announce();
    return 0;
}
