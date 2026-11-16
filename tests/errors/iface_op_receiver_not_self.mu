unit iface_op_receiver_not_self;

interface I {
    Self operator+(int a, Self b);
}

int main() { return 0; }
