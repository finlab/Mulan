// layout modifiers apply only to structs: not allowed on classes
unit packed_class;

packed class Bad {
    int x;
}

int main() { return 0; }
