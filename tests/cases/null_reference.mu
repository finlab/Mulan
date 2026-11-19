unit null_reference;

// Null-reference check (SPEC 3.8, 8.2): a method call, a field read and a field write through a null
// class handle — and a method call through a null interface value — throw
// std.exception.NullReferenceError; a non-null handle is unaffected.

extern int puts(const char* s);

class Dog {
    int age;

    void bark() {
        puts("bark");
    }

    int getAge() {
        return this.age;
    }
}

interface Speaker {
    void speak();
}

class Cat : Speaker {
    int lives;

    override void speak() {
        puts("meow");
    }
}

int main() {
    Dog d = null;
    if d == null { puts("handle is null"); } else { puts("handle is not null"); }

    int caught = 0;
    try {
        d.bark();
    } catch (e: NullReferenceError) {
        caught += 1;
        puts(e.message.cstr());
    }
    try {
        int a = d.getAge();
    } catch (e: NullReferenceError) {
        caught += 1;
        puts("read caught");
    }
    try {
        d.age = 3;
    } catch (e: NullReferenceError) {
        caught += 1;
        puts("write caught");
    }

    Speaker s = null;
    try {
        s.speak();
    } catch (e: NullReferenceError) {
        caught += 1;
        puts(e.message.cstr());
    }

    // a real object takes the normal path: the check must not fire
    Dog real = new Dog;
    real.age = 4;
    real.bark();
    int age = real.getAge();
    Speaker cat = new Cat;
    cat.speak();
    puts("normal path ok");

    if caught != 4 { puts("wrong catch count"); return 1; }
    if age != 4 { puts("wrong age"); return 2; }
    return 0;
}
