// align(N) cannot be smaller than the natural alignment (use packed to reduce alignment)
unit align_shrink;

align(2) struct Bad {
    int x;
    int y;
}

int main() { return 0; }
