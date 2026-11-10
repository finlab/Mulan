// align(16) 结构体切片的聚合初始化也会分配数据区（最多 8 字节对齐）
unit align_slice_init;

align(16) struct Wide {
    int x;
    int y;
}

int main() {
    Wide[] ws = { Wide(1, 2), Wide(3, 4) };
    return 0;
}
