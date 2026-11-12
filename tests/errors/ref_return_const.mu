unit rr3;

struct P {
    int x;
}

// `ref const` 借用不能以可变 `ref` 返回
ref int bad(ref const P p) {
    return p.x;
}

int main() {
    return 0;
}
