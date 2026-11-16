unit iface_generic_op_receiver;

interface I<T> {
    int operator<<(T b, byte c);
}

struct S { int x; }

int main() {
    I<S> i = null;
    return 0;
}
