// switch_basic.mu — switch：多值、半开区间、枚举穷尽、else、嵌套与 break 归属
unit switch_basic;

extern int putchar(int c);

enum Color { Red, Green, Blue }

void pi(int v) {
    if v == 0 {
        putchar('0');
        putchar('\n');
        return;
    }
    if v < 0 {
        putchar('-');
        v = -v;
    }
    int[20] d;
    int i = 0;
    while v > 0 {
        d[i] = v % 10;
        v /= 10;
        i += 1;
    }
    while i > 0 {
        i -= 1;
        putchar('0' + d[i]);
    }
    putchar('\n');
}

int classify(int code) {
    switch code {
        0: { return 0; }
        1, 2: { return 1; }
        3..10: { return 2; }
        else: { return -1; }
    }
}

// 枚举穷尽：不写 else 也必须覆盖全部成员（缺一个就编译错误）
int paint(Color c) {
    switch c {
        Red:   { return 11; }
        Green: { return 22; }
        Blue:  { return 33; }
    }
}

int main() {
    for i in 0..12 {
        pi(classify(i));
    }
    pi(paint(Red));
    pi(paint(Green));
    pi(paint(Blue));

    // 嵌套：switch 里的 break/continue 属于外层循环
    int n = 0;
    for i in 0..5 {
        switch i {
            0: { continue; }
            1: { n += 100; }
            2..4: { n += 10; }
            else: { n += 1; }
        }
        n += 1;
    }
    pi(n);

    // 字符区间（半开：'a'..'{' 含 'z'）
    byte ch = 'q';
    switch ch {
        'a'..'{': { pi(1); }
        else: { pi(2); }
    }

    // 空 switch 体 / 无 else 的整型 switch：不命中就跳过
    int k = 7;
    switch k { }
    switch k {
        3: { pi(9); }
    }

    // 被匹配值只求值一次
    return 0;
}
