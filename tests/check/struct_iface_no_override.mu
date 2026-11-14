// A struct satisfies an interface without `override`: the `:` list is the contract, and a value type
// cannot be subclassed, so the marker would not distinguish anything. A class still needs it on every
// interface method (it also covers base-class overrides).
unit struct_iface_no_override;

interface Animal {
    void speak();
    int legs();
}

// no `override` — accepted
struct Cat : Animal {
    int n;
    void speak() { }
    int legs() { return 4; }
}

class Dog : Animal {
    int n;
    override void speak() { }
    override int legs() { return 4; }
}

int main() {
    Cat c;
    c.speak();
    int l = c.legs();
    Dog d = new Dog();
    Animal a = d;
    int m = a.legs();
    return l + m;
}
