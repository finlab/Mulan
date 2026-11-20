// cast_class_interface — an explicit class-to-interface `as` is the same upcast implicit assignment
// allows (#147).
unit cast_class_interface;

extern int puts(const char* s);

interface IAnimal {
    int legs();
}

class Cat : IAnimal {
    public int n;
    Cat() { n = 4; }
    override int legs() { return n; }
}

int main() {
    Cat c = new Cat();
    IAnimal a = c as IAnimal;
    if a.legs() != 4 { puts("cast"); return 1; }
    puts("cast class interface ok");
    return 0;
}
