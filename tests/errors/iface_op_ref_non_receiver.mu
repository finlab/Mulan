unit iface_op_ref_non_receiver;

interface I {
    Self operator+(Self a, ref Self b);
}

int main() { return 0; }
