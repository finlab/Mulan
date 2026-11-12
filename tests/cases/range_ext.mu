// range_ext.mu — 区间扩展：`..=` 含上界、`step` 步进、负 step 倒序；花括号里的区间填充
unit range_ext;

extern int puts(const char* s);

struct Box {
    int[3] v;
}

int main() {
    int sum = 0;
    for i in 1..=5 {
        sum += i;
    }
    if sum == 15 {
        puts("inclusive ok");
    }

    sum = 0;
    for i in 0..10 step 2 {
        sum += i;
    }
    if sum == 20 {
        puts("step ok");
    }

    sum = 0;
    for i in 5..=1 step -1 {
        sum += i;
    }
    if sum == 15 {
        puts("reverse ok");
    }

    sum = 0;
    for i in 10..0 step -2 {
        sum += i;
    }
    if sum == 30 {
        puts("reverse step ok");
    }

    sum = 0;
    for i in 5..5 {
        sum += 100;
    }
    for i in 5..1 {
        sum += 100;
    }
    for i in 1..=0 {
        sum += 100;
    }
    if sum == 0 {
        puts("empty ok");
    }

    sum = 0;
    for i, x in 10..=12 {
        sum += i * x;
    }
    if sum == 35 {
        puts("index ok");
    }

    int[10] a = {1, 2, 5..9, 10};
    if a[0] == 1 and a[1] == 2 and a[2] == 5 and a[5] == 8 and a[6] == 10 and a[7] == 0 {
        puts("fill ok");
    }

    Box b = {{0..3}};
    if b.v[0] == 0 and b.v[2] == 2 {
        puts("struct fill ok");
    }

    int[2,3] m = {{0..3}, {3..6}};
    if m[0,2] == 2 and m[1,0] == 3 and m[1,2] == 5 {
        puts("multi fill ok");
    }

    int[4] e = {5..5};
    if e[0] == 0 and e[3] == 0 {
        puts("empty fill ok");
    }

    byte[4] bytes = {65..69};
    if bytes[0] == 65 and bytes[3] == 68 {
        puts("byte fill ok");
    }
    return 0;
}
