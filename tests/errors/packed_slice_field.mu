// packed 结构不能直接含动态数组（未对齐指针访问）
unit packed_slice_field;

packed struct Bad {
    byte tag;
    int[] xs;
}

int main() { return 0; }
