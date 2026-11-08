unit super_late; class A { A() { } } class B : A { B() { int x = 1; super(); } } int main() { return 0; }
