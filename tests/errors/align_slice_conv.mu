// align(16) 结构体的定长数组不能隐式转成动态数组（转换会分配数据区，最多 8 字节对齐）
unit align_slice_conv;

align(16) struct Wide {
    int x;
    int y;
}

int main() {
    Wide[2] arr;
    Wide[] ws = arr;
    return 0;
}
