// 布局修饰符只能用于 struct：class 上不允许
unit packed_class;

packed class Bad {
    int x;
}

int main() { return 0; }
