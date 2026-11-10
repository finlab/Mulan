// align(N) / packed 结构体不能作动态数组元素：数据区在 24 字节头部之后，最多 8 字节对齐
unit align_heap;

align(16) struct Wide {
    int x;
    int y;
}

int main() {
    Wide[] ws = new Wide[2];
    return 0;
}
