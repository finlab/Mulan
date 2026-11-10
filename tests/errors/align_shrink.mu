// align(N) 不能小于自然对齐（降对齐只能用 packed）
unit align_shrink;

align(2) struct Bad {
    int x;
    int y;
}

int main() { return 0; }
