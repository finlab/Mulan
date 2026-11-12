// layout modifiers apply only to structs: not allowed on enums
unit packed_enum;

packed enum Bad {
    Red,
    Green
}

int main() { return 0; }
