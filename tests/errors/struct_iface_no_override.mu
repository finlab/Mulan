unit struct_iface_no_override;

interface Animal {
    void speak();
}

struct Cat : Animal {
    void speak() { }
}

int main() {
    return 0;
}
