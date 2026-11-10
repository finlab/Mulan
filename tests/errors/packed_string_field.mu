// packed 结构不能直接含 string（未对齐指针访问）
unit packed_string_field;

packed struct Bad {
    byte tag;
    string s;
}

int main() { return 0; }
