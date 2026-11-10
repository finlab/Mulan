// 布局修饰符只能用于 struct：enum 上不允许
unit packed_enum;

packed enum Bad {
    Red,
    Green
}

int main() { return 0; }
