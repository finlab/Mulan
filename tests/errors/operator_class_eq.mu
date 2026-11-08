unit operator_class_eq; class C { int x; } bool operator==(C a, C b) { return a.x == b.x; } int main() { return 0; }
