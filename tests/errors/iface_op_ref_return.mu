unit iface_op_ref_return;

interface I {
    ref int operator<<(Self a, int b);
}

int main() { return 0; }
