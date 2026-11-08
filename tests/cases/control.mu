// control.mu — if/elif/else、while + break/continue、for i in 0..n
unit control;

extern int puts(readonly char* s);
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

int main() {
    // ---------- if / elif / else：按顺序取第一个成立的分支 ----------
    int score = 72;
    if score >= 90 {
        puts("grade A");
    } elif score >= 80 {
        puts("grade B");
    } elif score >= 70 {
        puts("grade C");
    } else {
        puts("grade F");
    }

    // 条件必须是 bool：!= 产生 bool
    if score != 100 {
        puts("not perfect");
    }

    // ---------- while + break / continue ----------
    // while true 合法（true 是 bool）；continue 跳回条件，break 退出
    int n = 0;
    int sum = 0;
    while true {
        n += 1;
        if n % 2 == 0 {
            continue;          // 跳过偶数
        }
        if n > 10 {
            break;             // 到 11 为止
        }
        sum += n;
    }
    print_int(sum); putchar('\n');   // 1+3+5+7+9 = 25
    print_int(n); putchar('\n');     // 11

    // ---------- while 累减 ----------
    int m = 100;
    int steps = 0;
    while m > 1 {
        m /= 2;
        steps += 1;
    }
    print_int(steps); putchar('\n'); // 100->50->25->12->6->3->1：6 步
    print_int(m); putchar('\n');     // 1

    // ---------- for i in 0..n（半开区间，n 不包含） ----------
    for i in 0..5 {
        print_int(i); putchar(' ');
    }
    putchar('\n');                   // 0 1 2 3 4

    // 嵌套循环：break 只退出内层
    for i in 0..3 {
        for j in 0..10 {
            if j == 2 {
                break;
            }
            print_int(i * 10 + j); putchar(' ');
        }
    }
    putchar('\n');                   // 0 1 10 11 20 21

    // for + continue；区间端点可以是表达式
    for i in 0..3 * 3 {
        if i % 3 == 0 {
            continue;
        }
        print_int(i); putchar(' ');
    }
    putchar('\n');                   // 1 2 4 5 7 8

    return 0;
}
