unit struct_iface_missing;

interface Animal {
    void speak();
}

struct Cat : Animal {
    int lives;

    override void run() { }
}

int main() {
    return 0;
}
