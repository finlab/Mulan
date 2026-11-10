// union_basic.mu — union 值类型：布局、成员读写、初始化、作字段/数组元素
unit union_basic;

extern int puts(const char* s);
extern int putchar(int c);

void print_int(int v) {
    if v == 0 {
        putchar('0');
        return;
    }
    if v < 0 {
        putchar('-');
        v = -v;
    }
    int[20] digits;
    int i = 0;
    while v > 0 {
        digits[i] = v % 10;
        v /= 10;
        i += 1;
    }
    while i > 0 {
        i -= 1;
        putchar('0' + digits[i]);
    }
}

void nl() { putchar('\n'); }

// 所有成员偏移 0；sizeof = max(成员 size) 向上对齐，alignof = 最大成员对齐
union Value {
    int i;
    float f;
    byte[8] raw;
}

union Small {
    byte b;
    ushort h;
}

packed union PackedU {
    byte a;
    int b;
}

struct Tagged {
    int tag;
    Value v;
}

int main() {
    // ---- 布局 ----
    print_int(sizeof(Value) as int); nl();    // 8（max(4,4,8)）
    print_int(alignof(Value) as int); nl();   // 4
    print_int(sizeof(Small) as int); nl();    // 2
    print_int(sizeof(PackedU) as int); nl();  // 4
    print_int(alignof(PackedU) as int); nl(); // 1（packed）

    // ---- 成员共享同一存储（C 语义：按字节重解释）----
    Value v;
    v.i = 0x41424344;
    print_int(v.i); nl();       // 1094861636
    print_int(v.raw[0]); nl();  // 0x44 = 68
    print_int(v.raw[1]); nl();  // 0x43 = 67（小端）

    // ---- 零初始化 ----
    Value z = {};
    print_int(z.i); nl();       // 0

    // ---- 具名初始化：只指定一个成员 ----
    Value w = { i = 7 };
    print_int(w.i); nl();       // 7

    // ---- 作为 struct 字段 ----
    Tagged t;
    t.tag = 3;
    t.v.i = 9;
    print_int(t.tag); nl();     // 3
    print_int(t.v.i); nl();     // 9
    print_int(sizeof(Tagged) as int); nl();  // 12

    // ---- 作数组元素 ----
    Value[3] arr;
    arr[0].i = 1;
    arr[1].i = 2;
    arr[2].i = 3;
    print_int(arr[0].i + arr[1].i + arr[2].i); nl();  // 6

    // ---- 赋值是字节拷贝 ----
    Value c = v;
    print_int(c.i); nl();       // 1094861636

    // ---- 取址（union 是 POD，不受 struct 的 U24 限制）----
    Value* pv = &v;
    print_int((*pv).i); nl();     // 1094861636
    return 0;
}
