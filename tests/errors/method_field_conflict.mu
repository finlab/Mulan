unit method_field_conflict; class C { int x; int x() { return 1; } } int main() { return 0; }
