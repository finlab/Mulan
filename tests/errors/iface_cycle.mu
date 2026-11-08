unit iface_cycle; interface A : B { } interface B : A { } int main() { return 0; }
