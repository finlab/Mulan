unit if_enum_body; enum E { #if 1 == 1 { A } #else { B } } int main() { return 0; }
