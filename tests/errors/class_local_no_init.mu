unit class_local_no_init;

// A class handle is a reference, not storage for an object: `Dog d;` must be initialized (SPEC 4).

class Dog {
    int age;
}

int main() {
    Dog d;
    return 0;
}
