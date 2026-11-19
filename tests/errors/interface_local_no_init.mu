unit interface_local_no_init;

// Same rule for an interface value (SPEC 4): `Speaker s;` is a null-valued handle, not an object.

interface Speaker {
    void speak();
}

class Cat : Speaker {
    override void speak() {
    }
}

int main() {
    Speaker s;
    return 0;
}
